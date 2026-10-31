/*
 * Improv Wi-Fi over serial (https://www.improv-wifi.com/serial/) on the Pico
 * W, as on the ESP32 (components/pairble/src/pairble_improv.c): the framing
 * is pairble_improv_proto.c, shared; this is the glue. Commands are answered
 * from pairble_loop(), and the two that wait - a connection attempt, a scan -
 * are answered when what they wait for is done, so nothing blocks.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <Arduino.h>
#include <WiFi.h>
#include "pairble.h"
#include "pairble_pico_internal.h"
extern "C" {
#include "pairble_improv_proto.h"  /* C, shared with the ESP32 */
}

#define BAD_PACKET 0xFF  /* queued in place of a command: answer INVALID_RPC */

static pairble_improv_write_t s_write;
static void *s_write_ctx;
static Stream *s_stream;           /* pairbleImprovSerial(): read from here */
static char s_url[128];
static char s_fw_name[48];
static char s_fw_version[32];
static improv_parser_t s_parser;
static improv_rpc_t s_rpc;
static bool s_rpc_pending;
static bool s_joining;             /* a WIFI_SETTINGS waits for its outcome */
static bool s_networks_pending;    /* a GET_NETWORKS waits for a scan */

static void send(const uint8_t *b, size_t n)
{
    if (n > 0 && s_write) s_write(b, n, s_write_ctx);
}

static void send_state(uint8_t state)
{
    uint8_t out[IMPROV_PACKET_MAX];
    send(out, improv_packet(IMPROV_TYPE_STATE, &state, 1, out));
}

static void send_error(uint8_t error)
{
    uint8_t out[IMPROV_PACKET_MAX];
    send(out, improv_packet(IMPROV_TYPE_ERROR, &error, 1, out));
}

/* The application's address, {ip} replaced by the board's. */
static void send_url(uint8_t command)
{
    char url[160] = "";
    const char *at = strstr(s_url, "{ip}");
    if (at) {
        snprintf(url, sizeof(url), "%.*s%s%s", (int)(at - s_url), s_url, WiFi.localIP().toString().c_str(), at + 4);
    } else {
        strlcpy(url, s_url, sizeof(url));
    }
    const char *strings[] = { url };
    uint8_t out[IMPROV_PACKET_MAX];
    send(out, improv_rpc_result(command, strings, url[0] ? 1 : 0, out));
}

static uint8_t current_state(void)
{
    if (s_joining || pb_shared.sta_state == PB_STA_CONNECTING) return IMPROV_STATE_PROVISIONING;
    return pairble_is_online() ? IMPROV_STATE_PROVISIONED : IMPROV_STATE_READY;
}

static void send_info(void)
{
#if defined(PICO_RP2350)
    const char *chip = "RP2350";
#else
    const char *chip = "RP2040";
#endif
    const char *strings[] = { s_fw_name[0] ? s_fw_name : "pairble", s_fw_version[0] ? s_fw_version : "0.0.0", chip,
                              pairble_device_name() };
    uint8_t out[IMPROV_PACKET_MAX];
    send(out, improv_rpc_result(IMPROV_CMD_GET_INFO, strings, 4, out));
}

static void send_networks(void)
{
    uint8_t out[IMPROV_PACKET_MAX];
    for (int i = 0; i < pb_shared.scan_count; i++) {  /* one name each, strongest first */
        const pb_scan_entry_t *e = &pb_shared.scan[i];
        char rssi[8];
        snprintf(rssi, sizeof(rssi), "%d", e->rssi);
        const char *strings[] = { e->ssid, rssi, e->auth == 0 ? "NO" : "YES" };
        send(out, improv_rpc_result(IMPROV_CMD_GET_NETWORKS, strings, 3, out));
    }
    send(out, improv_rpc_result(IMPROV_CMD_GET_NETWORKS, NULL, 0, out));  /* the end of the list */
}

static void handle(const improv_rpc_t *rpc)
{
    switch (rpc->command) {
    case IMPROV_CMD_WIFI_SETTINGS: {
        char ssid[33], password[64];
        if (!improv_wifi_settings(rpc, ssid, password)) {
            send_error(IMPROV_ERROR_INVALID_RPC);
            return;
        }
        if (pb_shared.sta_state == PB_STA_CONNECTING || pb_shared.want_apply) {
            /* Another attempt is running (a phone's, pairble_set_network()). */
            memset(password, 0, sizeof(password));
            send_error(IMPROV_ERROR_UNABLE_TO_CONNECT);
            return;
        }
        send_state(IMPROV_STATE_PROVISIONING);
        pb_request_network(ssid, password);
        memset(password, 0, sizeof(password));
        s_joining = true;
        return;
    }
    case IMPROV_CMD_GET_STATE:
        send_state(current_state());
        if (current_state() == IMPROV_STATE_PROVISIONED) send_url(IMPROV_CMD_WIFI_SETTINGS);
        return;
    case IMPROV_CMD_GET_INFO:
        send_info();
        return;
    case IMPROV_CMD_GET_NETWORKS:
        pb_request_scan();
        s_networks_pending = true;
        return;
    case BAD_PACKET:
        send_error(IMPROV_ERROR_INVALID_RPC);
        return;
    default:
        send_error(IMPROV_ERROR_UNKNOWN_RPC);
    }
}

void pb_improv_loop(void)
{
    if (s_stream) {
        uint8_t buf[64];
        size_t n = 0;
        while (n < sizeof(buf) && s_stream->available() > 0) buf[n++] = (uint8_t)s_stream->read();
        pairble_improv_input(buf, n);
    }
    if (s_rpc_pending && !s_joining && !s_networks_pending) {
        s_rpc_pending = false;
        handle(&s_rpc);
    }
    if (s_networks_pending && !pb_scanning()) {
        s_networks_pending = false;
        send_networks();
    }
    if (s_joining && pb_shared.sta_state != PB_STA_CONNECTING && !pb_shared.want_apply) {
        s_joining = false;
        if (pb_shared.sta_state == PB_STA_CONNECTED) {
            send_state(IMPROV_STATE_PROVISIONED);
            send_url(IMPROV_CMD_WIFI_SETTINGS);
            /* Improv has its answer: open setup closes as after the SoftAP
             * page. */
            pairble_lock();
            pb_shared.result_delivered_at = millis();
            pb_shared.result_over_http = true;
            pb_shared.result_delivered = true;
            pairble_unlock();
        } else {
            send_error(IMPROV_ERROR_UNABLE_TO_CONNECT);
            send_state(IMPROV_STATE_READY);
        }
    }
}

pairble_err_t pairble_improv_start(pairble_improv_write_t write, void *ctx, const char *url)
{
    if (!write) return PAIRBLE_ERR_INVALID_ARG;
    if (s_write) return PAIRBLE_ERR_INVALID_STATE;
    s_write = write;
    s_write_ctx = ctx;
    strlcpy(s_url, url ? url : "", sizeof(s_url));
    return PAIRBLE_OK;
}

void pairble_improv_set_firmware(const char *name, const char *version)
{
    strlcpy(s_fw_name, name ? name : "", sizeof(s_fw_name));
    strlcpy(s_fw_version, version ? version : "", sizeof(s_fw_version));
}

void pairble_improv_input(const uint8_t *data, size_t len)
{
    static improv_rpc_t rpc;
    if (!s_write) return;
    for (size_t i = 0; i < len; i++) {
        improv_event_t e = improv_parse_byte(&s_parser, data[i], &rpc);
        if (e == IMPROV_BAD_PACKET) {
            rpc.command = BAD_PACKET;
            rpc.len = 0;
        }
        /* One at a time: Improv clients wait for the answer before the next
         * command, so one arriving meanwhile is dropped, as on the ESP32. */
        if (e != IMPROV_NONE && !s_rpc_pending) {
            s_rpc = rpc;
            s_rpc_pending = true;
        }
    }
}

bool pairble_improv_parsing(void)
{
    return s_parser.len > 0;
}

static void stream_write(const uint8_t *data, size_t len, void *ctx)
{
    static_cast<Stream *>(ctx)->write(data, len);
}

void pairbleImprovSerial(Stream &serial, const char *url, const char *firmware, const char *version)
{
    pairble_improv_set_firmware(firmware, version);
    if (pairble_improv_start(stream_write, &serial, url) == PAIRBLE_OK) s_stream = &serial;
}
