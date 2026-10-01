/*
 * SoftAP fallback: the device's own Wi-Fi network, offered at the same time as
 * BLE, for phones that cannot use Bluetooth - an iPhone with no internet to
 * fetch the App Clip, a browser without Web Bluetooth, a laptop.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The network is WPA2 with the proof of possession as its password, so joining
 * it already proves possession of the label and encrypts the link. That is why
 * this side needs no protocomm session: the API below is plain HTTP, and
 * credentials go into the same provisioning manager as BLE ones, through
 * network_prov_mgr_configure_wifi_sta(), which exists for exactly this.
 *
 * Everything answers only to clients of the SoftAP. Once the device has joined
 * the home network the HTTP server would otherwise be reachable from there too.
 */

#include <string.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "network_provisioning/manager.h"
#include "pairble_internal.h"
#include "pairble_text.h"

static const char *TAG = PAIRBLE_TAG;

static httpd_handle_t s_server;
static esp_netif_t *s_ap_netif;
static char s_name[33];

/* The setup page, built by packages/web and embedded by CMake (EMBED_FILES). */
extern const uint8_t setup_page_gz_start[] asm("_binary_setup_html_gz_start");
extern const uint8_t setup_page_gz_end[] asm("_binary_setup_html_gz_end");

/* ---- request filtering -------------------------------------------------- */

/* The IPv4 address in `addr`, which the server's IPv6 socket reports
 * v4-mapped (::ffff:a.b.c.d). 0 if it is not IPv4. */
static uint32_t ipv4_of(const struct sockaddr_storage *addr)
{
    if (addr->ss_family == AF_INET) {
        return ((const struct sockaddr_in *)addr)->sin_addr.s_addr;
    }
#if CONFIG_LWIP_IPV6
    if (addr->ss_family == AF_INET6) {
        const struct sockaddr_in6 *six = (const struct sockaddr_in6 *)addr;
        if (six->sin6_addr.un.u32_addr[0] == 0 && six->sin6_addr.un.u32_addr[1] == 0 &&
            six->sin6_addr.un.u32_addr[2] == PP_HTONL(0x0000FFFFUL)) {
            return six->sin6_addr.un.u32_addr[3];
        }
    }
#endif
    return 0;
}

/* True if the request came from a client of the SoftAP.
 *
 * Checking the local address alone is not enough: lwIP accepts a packet for
 * any of the device's addresses on any interface, so a host on the home
 * network could route 192.168.4.1 via the device's address there and pass.
 * It cannot also appear to come from inside the AP's subnet - the replies
 * would go out of the AP interface and the TCP handshake never completes. */
static bool from_softap(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    struct sockaddr_storage local, peer;
    socklen_t len = sizeof(local);
    if (getsockname(fd, (struct sockaddr *)&local, &len) != 0) {
        return false;
    }
    len = sizeof(peer);
    if (getpeername(fd, (struct sockaddr *)&peer, &len) != 0) {
        return false;
    }
    esp_netif_ip_info_t ap;
    if (esp_netif_get_ip_info(s_ap_netif, &ap) != ESP_OK) {
        return false;
    }
    uint32_t to = ipv4_of(&local), from = ipv4_of(&peer);
    return to == ap.ip.addr && from != 0 && (from & ap.netmask.addr) == (ap.ip.addr & ap.netmask.addr);
}

#define ONLY_FROM_SOFTAP(req)                                                        \
    do {                                                                             \
        if (!from_softap(req)) {                                                     \
            return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, NULL);              \
        }                                                                            \
    } while (0)

/* ---- small helpers ------------------------------------------------------ */

static esp_err_t send_json(httpd_req_t *req, const char *json)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
}

/* ---- API ---------------------------------------------------------------- */

#define SCAN_ROW_MAX (6 * 32 + 80)
#define SCAN_JSON_MAX 4096

static esp_err_t api_scan(httpd_req_t *req)
{
    ONLY_FROM_SOFTAP(req);
    wifi_scan_config_t scan = { .show_hidden = false };
    int64_t started = esp_timer_get_time();
    if (esp_wifi_scan_start(&scan, true) != ESP_OK) {
        /* Most likely a scan already running for a BLE client; retry. */
        httpd_resp_set_status(req, "503 Service Unavailable");
        return send_json(req, "{\"error\":\"busy\"}");
    }
    uint16_t count = 20;
    wifi_ap_record_t *records = calloc(count, sizeof(*records));
    char *out = malloc(SCAN_JSON_MAX);
    if (records == NULL || out == NULL) {
        free(records);
        free(out);
        return httpd_resp_send_500(req);
    }
    esp_wifi_scan_get_ap_records(&count, records);
    /* Logged because it is a UX number: the person watches a progress bar for
     * exactly this long, and it is much longer than over BLE. */
    ESP_LOGI(TAG, "scan over SoftAP: %d networks in %lld ms", count,
             (long long)((esp_timer_get_time() - started) / 1000));
    size_t o = snprintf(out, SCAN_JSON_MAX, "[");
    for (int i = 0; i < count && o < SCAN_JSON_MAX - SCAN_ROW_MAX; i++) {
        char ssid[6 * 32 + 3];  /* every byte escaped as \u00XX */
        pairble_json_string(ssid, sizeof(ssid), (const char *)records[i].ssid);
        o += snprintf(out + o, SCAN_JSON_MAX - o, "%s{\"ssid\":%s,\"rssi\":%d,\"channel\":%d,\"auth\":%d}",
                      i ? "," : "", ssid, records[i].rssi, records[i].primary, records[i].authmode);
    }
    snprintf(out + o, SCAN_JSON_MAX - o, "]");
    esp_err_t err = send_json(req, out);
    free(records);
    free(out);
    return err;
}

/* Percent-encoded, a 32-byte SSID and a 63-byte passphrase come to at most
 * 5 + 96 + 10 + 189 bytes. */
#define CONNECT_BODY_MAX 320

static esp_err_t api_connect(httpd_req_t *req)
{
    ONLY_FROM_SOFTAP(req);
    /* A header a plain HTML form cannot send: another page open in the phone's
     * browser cannot post here without a CORS preflight, which it won't pass. */
    char marker[4];
    if (httpd_req_get_hdr_value_str(req, "X-Pairble", marker, sizeof(marker)) != ESP_OK ||
        strcmp(marker, "1") != 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing X-Pairble header");
    }
    if (req->content_len == 0 || req->content_len > CONNECT_BODY_MAX) {
        httpd_resp_set_status(req, "413 Payload Too Large");
        return send_json(req, "{\"error\":\"body-size\"}");
    }
    char body[CONNECT_BODY_MAX + 1] = {0};
    size_t got = 0;
    while (got < req->content_len) {  /* one recv returns what TCP has so far */
        int n = httpd_req_recv(req, body + got, req->content_len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (n <= 0) {
            return ESP_FAIL;  /* the connection is gone; nothing to answer */
        }
        got += n;
    }

    char raw[3 * 64 + 8];  /* room for a percent-encoded 63-byte passphrase */
    int ssid_len = -1, pass_len = 0;
    if (httpd_query_key_value(body, "ssid", raw, sizeof(raw)) == ESP_OK) {
        ssid_len = pairble_url_decode(raw);
    }
    wifi_config_t cfg = { 0 };
    if (ssid_len < 1 || ssid_len > (int)sizeof(cfg.sta.ssid)) {
        memset(body, 0, sizeof(body));
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid must be 1-32 bytes");
    }
    memcpy(cfg.sta.ssid, raw, ssid_len);  /* exactly 32 bytes carries no NUL, and needs none */
    esp_err_t found = httpd_query_key_value(body, "password", raw, sizeof(raw));
    if (found == ESP_OK) {
        pass_len = pairble_url_decode(raw);
    } else if (found != ESP_ERR_NOT_FOUND) {
        pass_len = -1;  /* truncated: would silently become a different password */
    }
    if (pass_len < 0 || pass_len > 63) {
        memset(raw, 0, sizeof(raw));
        memset(body, 0, sizeof(body));
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "password must be at most 63 bytes");
    }
    memcpy(cfg.sta.password, raw, pass_len);

    /* The same path BLE credentials take, so failure handling, retries and the
     * status the device reports are identical whichever way they arrived. */
    esp_err_t err = network_prov_mgr_configure_wifi_sta(&cfg);
    memset(&cfg, 0, sizeof(cfg));
    memset(raw, 0, sizeof(raw));
    memset(body, 0, sizeof(body));
    if (err != ESP_OK) {
        /* An attempt is running or has already succeeded: its result stands. */
        httpd_resp_set_status(req, "409 Conflict");
        return send_json(req, "{\"error\":\"not-accepted\"}");
    }
    /* Before answering, so a status poll after this response cannot read the
     * previous attempt's failure; the manager reports CRED_RECV asynchronously. */
    pairble_result_set_connecting();
    return send_json(req, "{\"ok\":true}");
}

static esp_err_t api_status(httpd_req_t *req)
{
    ONLY_FROM_SOFTAP(req);
    char out[160];
    pairble_result_t r = pairble_current_result();
    switch (r.state) {
    case PAIRBLE_RESULT_CONNECTING:
        return send_json(req, "{\"state\":\"connecting\"}");
    case PAIRBLE_RESULT_FAILED:
        snprintf(out, sizeof(out), "{\"state\":\"failed\",\"reason\":\"%s\"}",
                 r.reason == PAIRBLE_FAIL_WRONG_PASSWORD ? "wrong-password" : "network-not-found");
        return send_json(req, out);
    case PAIRBLE_RESULT_CONNECTED:
        snprintf(out, sizeof(out), "{\"state\":\"connected\",\"ip\":\"%s\"}", r.ip);
        esp_err_t sent = send_json(req, out);
        pairble_result_delivered();
        return sent;
    default:
        return send_json(req, "{\"state\":\"idle\"}");
    }
}

static esp_err_t api_info(httpd_req_t *req)
{
    ONLY_FROM_SOFTAP(req);
    char name[80], out[120];
    pairble_json_string(name, sizeof(name), s_name);
    snprintf(out, sizeof(out), "{\"name\":%s}", name);
    return send_json(req, out);
}

/* ---- the page ----------------------------------------------------------- */

/* The same single-file page that runs on pairble.com. Served over plain HTTP
 * it is not a secure context, has no Web Bluetooth, and picks the API above -
 * nothing in the page is specific to either place. */
static esp_err_t page_root(httpd_req_t *req)
{
    ONLY_FROM_SOFTAP(req);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, (const char *)setup_page_gz_start, setup_page_gz_end - setup_page_gz_start);
}

/* ---- captive portal ------------------------------------------------------
 * A convenience, never the documented path: iOS 26 has been reported not to
 * pop the portal sheet, and custom DNS or a VPN defeats it entirely. The label
 * and the page always say "open http://192.168.4.1" as well. */

#if CONFIG_PAIRBLE_CAPTIVE_PORTAL
/* Every unknown URL - which is every connectivity probe: Apple's
 * /hotspot-detect.html, Android's /generate_204, Windows' /connecttest.txt -
 * is redirected to the page. A redirect, rather than the answer each probe
 * expects, is what tells the phone there is a portal to show. */
static esp_err_t redirect_to_page(httpd_req_t *req, httpd_err_code_t err)
{
    if (!from_softap(req)) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, NULL);
    }
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, NULL, 0);
}

static volatile bool s_dns_running;
static TaskHandle_t s_dns_task;

/* Answers every A query with the access point's own address, so any name a
 * phone looks up leads here. AAAA and everything else get an empty answer.
 * Additional records (EDNS) are dropped rather than echoed. */
static void dns_task(void *arg)
{
    uint32_t ip = (uint32_t)(uintptr_t)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = ip };
    struct timeval timeout = { .tv_sec = 1 };
    if (sock < 0 || bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGW(TAG, "captive portal DNS unavailable");
        if (sock >= 0) close(sock);
        s_dns_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    uint8_t buf[512];
    while (s_dns_running) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &from_len);
        int len = pairble_dns_answer(buf, n, sizeof(buf), ip);
        if (len < 0) {
            continue;
        }
        sendto(sock, buf, len, 0, (struct sockaddr *)&from, from_len);
    }
    close(sock);
    s_dns_task = NULL;
    vTaskDelete(NULL);
}
#endif

/* ---- lifecycle ---------------------------------------------------------- */

esp_err_t pairble_softap_start(const char *ssid, const char *password)
{
    strlcpy(s_name, ssid, sizeof(s_name));
    s_ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (s_ap_netif == NULL) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
    }

    wifi_config_t ap = { 0 };
    strlcpy((char *)ap.ap.ssid, ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(ssid);
    strlcpy((char *)ap.ap.password, password, sizeof(ap.ap.password));
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.max_connection = 2;
    ap.ap.channel = 1;
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_AP, &ap);
    }
    memset(&ap, 0, sizeof(ap));
    if (err != ESP_OK) {
        /* Never leave the AP up on the driver's default (open) settings. */
        ESP_LOGE(TAG, "SoftAP config failed: %s", esp_err_to_name(err));
        esp_wifi_set_mode(WIFI_MODE_STA);
        return err;
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 8;
    cfg.max_open_sockets = 5;
    cfg.lru_purge_enable = true;
    cfg.stack_size = 6144;
    err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        /* Usually no internal RAM left for its task. Take the network down
         * again: an access point with nothing behind it only confuses. */
        ESP_LOGE(TAG, "HTTP server failed: %s", esp_err_to_name(err));
        s_server = NULL;
        esp_wifi_set_mode(WIFI_MODE_STA);
        return err;
    }
    const httpd_uri_t uris[] = {
        { .uri = "/",            .method = HTTP_GET,  .handler = page_root },
        { .uri = "/api/info",    .method = HTTP_GET,  .handler = api_info },
        { .uri = "/api/scan",    .method = HTTP_GET,  .handler = api_scan },
        { .uri = "/api/connect", .method = HTTP_POST, .handler = api_connect },
        { .uri = "/api/status",  .method = HTTP_GET,  .handler = api_status },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(s_server, &uris[i]);
    }
#if CONFIG_PAIRBLE_CAPTIVE_PORTAL
    httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, redirect_to_page);
    esp_netif_ip_info_t ap_ip;
    esp_netif_get_ip_info(s_ap_netif, &ap_ip);
    s_dns_running = true;
    xTaskCreate(dns_task, "pairble_dns", 4096, (void *)(uintptr_t)ap_ip.ip.addr, 4, &s_dns_task);
#endif
    ESP_LOGI(TAG, "SoftAP \"%s\" up at 192.168.4.1 alongside BLE; free heap %u B",
             ssid, (unsigned)esp_get_free_heap_size());
    return ESP_OK;
}

void pairble_softap_stop(void)
{
    if (s_server != NULL) {
        httpd_stop(s_server);
        s_server = NULL;
        ESP_LOGI(TAG, "SoftAP setup closed");
    }
#if CONFIG_PAIRBLE_CAPTIVE_PORTAL
    /* The task notices within its one-second receive timeout. Wait for it:
     * setup reopened sooner would start a second one that cannot bind. */
    s_dns_running = false;
    for (int waited = 0; s_dns_task != NULL && waited < 1500; waited += 50) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
#endif
}
