/*
 * SoftAP fallback on the Pico W: the board's own Wi-Fi network next to BLE,
 * for phones that cannot use Bluetooth. The same as on the ESP32
 * (components/pairble/src/pairble_softap.c): WPA2 with the proof of
 * possession as the password, the setup page from flash at 192.168.4.1, the
 * plain HTTP API the page uses there, and a captive portal.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include "pairble_pico_internal.h"
extern "C" {
#include "pairble_text.h"  /* C, shared with the ESP32 */
}

#if CONFIG_PAIRBLE_SOFTAP

extern "C" const uint8_t _binary_setup_html_gz_start[];
extern "C" const uint8_t _binary_setup_html_gz_end[];

static const IPAddress AP_IP(192, 168, 4, 1);
static const IPAddress AP_MASK(255, 255, 255, 0);
static const uint32_t CLIENT_RECENT_MS = 30000;

static WebServer *s_server;
static WiFiUDP s_dns;
static bool s_running;
static char s_name[33];
static uint32_t s_last_request;

/* Only clients of the access point: once on the home network the server
 * would otherwise answer there too. As pairble_softap.c, from_softap(). */
static bool from_softap(void)
{
    WiFiClient c = s_server->client();
    IPAddress local = c.localIP(), peer = c.remoteIP();
    bool ok = local == AP_IP && (uint32_t(peer) & uint32_t(AP_MASK)) == (uint32_t(AP_IP) & uint32_t(AP_MASK));
    if (ok) s_last_request = millis() | 1;
    return ok;
}

static void send_json(int code, const char *json)
{
    s_server->sendHeader("Cache-Control", "no-store");
    s_server->send(code, "application/json", json);
}

#define ONLY_FROM_SOFTAP()                          \
    do {                                            \
        if (!from_softap()) {                       \
            s_server->send(404, "text/plain", "");  \
            return;                                 \
        }                                           \
    } while (0)

static void page_root(void)
{
    ONLY_FROM_SOFTAP();
    s_server->sendHeader("Content-Encoding", "gzip");
    s_server->sendHeader("Cache-Control", "no-cache");
    s_server->send(200, "text/html; charset=utf-8", (const char *)_binary_setup_html_gz_start,
                   _binary_setup_html_gz_end - _binary_setup_html_gz_start);
}

static void api_info(void)
{
    ONLY_FROM_SOFTAP();
    char name[80], out[120];
    pairble_json_string(name, sizeof(name), s_name);
    snprintf(out, sizeof(out), "{\"name\":%s}", name);
    send_json(200, out);
}

/* The list kept fresh while setup is open; 503 while there is none yet,
 * which the page retries (packages/web/app/local.ts). */
static void api_scan(void)
{
    ONLY_FROM_SOFTAP();
    if (!pb_shared.scan_done) {
        pb_request_scan();
        send_json(503, "{\"error\":\"busy\"}");
        return;
    }
    const size_t cap = 4096;
    char *out = (char *)malloc(cap);
    if (!out) {
        send_json(503, "{\"error\":\"busy\"}");
        return;
    }
    size_t o = snprintf(out, cap, "[");
    pairble_lock();
    for (int i = 0; i < pb_shared.scan_count && o < cap - 280; i++) {
        const pb_scan_entry_t *e = &pb_shared.scan[i];
        char ssid[6 * 32 + 3];
        pairble_json_string(ssid, sizeof(ssid), e->ssid);
        o += snprintf(out + o, cap - o, "%s{\"ssid\":%s,\"rssi\":%d,\"channel\":%d,\"auth\":%d}", i ? "," : "",
                      ssid, e->rssi, e->channel, e->auth);
    }
    pairble_unlock();
    snprintf(out + o, cap - o, "]");
    send_json(200, out);
    free(out);
}

static void api_connect(void)
{
    ONLY_FROM_SOFTAP();
    /* A header a plain HTML form cannot send: another page cannot post here
     * without a CORS preflight, which it won't pass. */
    if (s_server->header("X-Pairble") != "1") {
        s_server->send(400, "text/plain", "missing X-Pairble header");
        return;
    }
    String ssid = s_server->arg("ssid"), pass = s_server->arg("password");
    if (ssid.length() < 1 || ssid.length() > 32) {
        s_server->send(400, "text/plain", "ssid must be 1-32 bytes");
        return;
    }
    if (pass.length() > 63) {
        s_server->send(400, "text/plain", "password must be at most 63 bytes");
        return;
    }
    if (pb_shared.sta_state == PB_STA_CONNECTING || pb_shared.want_apply) {
        send_json(409, "{\"error\":\"not-accepted\"}");
        return;
    }
    /* The way Bluetooth credentials take: joined from pairble_loop(), after
     * this answer has gone out. */
    pairble_lock();
    strcpy(pb_shared.new_ssid, ssid.c_str());
    strcpy(pb_shared.new_pass, pass.c_str());
    pb_shared.sta_state = PB_STA_CONNECTING;
    pb_shared.want_apply = true;
    pairble_unlock();
    send_json(200, "{\"ok\":true}");
}

static void api_status(void)
{
    ONLY_FROM_SOFTAP();
    char out[160];
    switch (pb_shared.sta_state) {
    case PB_STA_CONNECTING:
        send_json(200, "{\"state\":\"connecting\"}");
        return;
    case PB_STA_FAILED:
        snprintf(out, sizeof(out), "{\"state\":\"failed\",\"reason\":\"%s\"}",
                 pb_shared.sta_reason == PB_REASON_AUTH ? "wrong-password" : "network-not-found");
        send_json(200, out);
        return;
    case PB_STA_CONNECTED:
        snprintf(out, sizeof(out), "{\"state\":\"connected\",\"ip\":\"%s\"}", pb_shared.ip);
        send_json(200, out);
        if (!pb_shared.result_delivered) {
            pb_shared.result_delivered_at = millis();
            pb_shared.result_over_http = true;
            pb_shared.result_delivered = true;
        }
        return;
    default:
        send_json(200, "{\"state\":\"idle\"}");
    }
}

/* Every other address - the phone's connectivity checks among them - is sent
 * to the page, so the system offers to open it. */
static void redirect_to_page(void)
{
    if (!from_softap()) {
        s_server->send(404, "text/plain", "");
        return;
    }
    s_server->sendHeader("Location", "http://192.168.4.1/");
    s_server->send(302, "text/plain", "");
}

void pb_softap_start(const char *name, const char *pop)
{
    if (s_running) return;
    strlcpy(s_name, name, sizeof(s_name));
    /* First: outside the AP modes softAPConfig() sets these as the station's
     * static address instead, and the home network would then be joined as
     * 192.168.4.1, without DHCP. AP_STA also keeps a station that is up. */
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAPConfig(AP_IP, AP_IP, AP_MASK);
    /* On the home network's channel when there is one: one radio, which
     * would otherwise keep hopping between the two. */
    if (!WiFi.beginAP(name, pop, WiFi.connected() ? WiFi.channel() : 0)) {
        Serial.println("pairble: SoftAP did not start");
        return;
    }
    {
        s_server = new WebServer(80);
        s_server->on("/", HTTP_GET, page_root);
        s_server->on("/api/info", HTTP_GET, api_info);
        s_server->on("/api/scan", HTTP_GET, api_scan);
        s_server->on("/api/connect", HTTP_POST, api_connect);
        s_server->on("/api/status", HTTP_GET, api_status);
        s_server->onNotFound(redirect_to_page);
        s_server->collectHeaders("X-Pairble");
    }
    s_server->begin();
    s_dns.begin(53);
    s_running = true;
    Serial.printf("pairble: SoftAP \"%s\" up at 192.168.4.1 alongside BLE\n", name);
}

void pb_softap_stop(void)
{
    if (!s_running) return;
    s_dns.stop();
    s_server->stop();
    delete s_server;
    s_server = nullptr;
    /* Joining a network leaves WiFiClass in STA mode with the access point
     * still up, and softAPdisconnect() would then take the station down
     * instead - the network just joined. */
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAPdisconnect(false);
    s_running = false;
}

void pb_softap_loop(void)
{
    if (!s_running) return;
    s_server->handleClient();
    int n = s_dns.parsePacket();
    if (n > 0) {
        uint8_t buf[512];
        n = s_dns.read(buf, sizeof(buf) - 16 > (size_t)n ? n : sizeof(buf) - 16);
        int len = pairble_dns_answer(buf, n, sizeof(buf), uint32_t(AP_IP));
        if (len > 0) {
            s_dns.beginPacket(s_dns.remoteIP(), s_dns.remotePort());
            s_dns.write(buf, len);
            s_dns.endPacket();
        }
    }
}

bool pb_softap_running(void) { return s_running; }

bool pb_softap_has_clients(void)
{
    return s_running && (WiFi.softAPgetStationNum() > 0 ||
                         (s_last_request && pb_since(s_last_request) < CLIENT_RECENT_MS));
}

#else

void pb_softap_start(const char *, const char *) {}
void pb_softap_stop(void) {}
void pb_softap_loop(void) {}
bool pb_softap_running(void) { return false; }
bool pb_softap_has_clients(void) { return false; }

#endif
