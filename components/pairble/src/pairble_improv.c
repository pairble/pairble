/*
 * Improv Wi-Fi over serial (https://www.improv-wifi.com/serial/): the ESP Web
 * Tools installer, improv-wifi.com and Home Assistant can set the network
 * over USB, right after flashing.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * A network from Improv goes through pairble_set_network(), so it is treated
 * like one from a phone: the stored network stays until the new one works.
 * The serial port has no security of its own - whoever holds the cable holds
 * the device - which is what Improv assumes too.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "pairble_improv_proto.h"
#include "pairble_internal.h"
#if CONFIG_PAIRBLE_IMPROV_UART
#include "driver/uart.h"
#endif

static const char *TAG = PAIRBLE_TAG;

#define BAD_PACKET 0xFF  /* queued in place of a command: answer INVALID_RPC */

static pairble_improv_write_t s_write;
static void *s_write_ctx;
static char s_url[128];
static char s_fw_name[48];
static char s_fw_version[32];
static improv_parser_t s_parser;
static QueueHandle_t s_queue;

static void send(const uint8_t *b, size_t n)
{
    if (n > 0) {
        s_write(b, n, s_write_ctx);
    }
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

/* The address the installer offers to open once the device is online: the
 * application's, with {ip} replaced by the device's address. */
static void send_url(uint8_t command)
{
    char url[160] = "";
    const char *at = strstr(s_url, "{ip}");
    if (at != NULL) {
        esp_netif_ip_info_t ip = { 0 };
        esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (sta != NULL) {
            esp_netif_get_ip_info(sta, &ip);
        }
        snprintf(url, sizeof(url), "%.*s" IPSTR "%s", (int)(at - s_url), s_url, IP2STR(&ip.ip), at + 4);
    } else {
        strlcpy(url, s_url, sizeof(url));
    }
    const char *strings[] = { url };
    uint8_t out[IMPROV_PACKET_MAX];
    send(out, improv_rpc_result(command, strings, url[0] ? 1 : 0, out));
}

static uint8_t current_state(void)
{
    if (pairble_current_result().state == PAIRBLE_RESULT_CONNECTING) {
        return IMPROV_STATE_PROVISIONING;
    }
    return pairble_is_online() ? IMPROV_STATE_PROVISIONED : IMPROV_STATE_READY;
}

static void send_info(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    /* "esp32s3" -> "ESP32-S3" */
    char chip[16];
    const char *target = CONFIG_IDF_TARGET;
    size_t o = 0;
    for (size_t i = 0; target[i] != '\0' && o < sizeof(chip) - 2; i++) {
        if (i == 5) {
            chip[o++] = '-';
        }
        chip[o++] = (char)(target[i] >= 'a' && target[i] <= 'z' ? target[i] - 32 : target[i]);
    }
    chip[o] = '\0';
    const char *strings[] = { s_fw_name[0] ? s_fw_name : app->project_name,
                              s_fw_version[0] ? s_fw_version : app->version, chip, pairble_device_name() };
    uint8_t out[IMPROV_PACKET_MAX];
    send(out, improv_rpc_result(IMPROV_CMD_GET_INFO, strings, 4, out));
}

static void send_networks(void)
{
    uint8_t out[IMPROV_PACKET_MAX];
    wifi_scan_config_t scan = { .show_hidden = false };
    esp_err_t err = ESP_FAIL;
    /* The station may be connecting, or setup scanning for a phone: retry. */
    for (int i = 0; i < 6 && (err = esp_wifi_scan_start(&scan, true)) != ESP_OK; i++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    if (err == ESP_OK) {
        uint16_t count = 20;
        wifi_ap_record_t *aps = calloc(count, sizeof(*aps));
        if (aps != NULL) {
            esp_wifi_scan_get_ap_records(&count, aps);  /* strongest first */
            for (int i = 0; i < count; i++) {
                const char *ssid = (const char *)aps[i].ssid;
                bool seen = ssid[0] == '\0';  /* hidden: nothing to show */
                for (int j = 0; j < i && !seen; j++) {
                    seen = strcmp(ssid, (const char *)aps[j].ssid) == 0;  /* another access point */
                }
                if (seen) {
                    continue;
                }
                char rssi[8];
                snprintf(rssi, sizeof(rssi), "%d", aps[i].rssi);
                const char *strings[] = { ssid, rssi, aps[i].authmode == WIFI_AUTH_OPEN ? "NO" : "YES" };
                send(out, improv_rpc_result(IMPROV_CMD_GET_NETWORKS, strings, 3, out));
            }
            free(aps);
        } else {
            esp_wifi_clear_ap_list();
        }
    }
    send(out, improv_rpc_result(IMPROV_CMD_GET_NETWORKS, NULL, 0, out));  /* the end of the list */
}

static void set_network(const improv_rpc_t *rpc)
{
    char ssid[33], password[64];
    if (!improv_wifi_settings(rpc, ssid, password)) {
        send_error(IMPROV_ERROR_INVALID_RPC);
        return;
    }
    send_state(IMPROV_STATE_PROVISIONING);
    /* Over the cable a new network is always welcome: open setup for it. */
    if (!pairble_is_setup_open()) {
        pairble_open_setup();
        for (int waited = 0; !pairble_is_setup_open() && waited < 20000; waited += 100) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    esp_err_t err = pairble_set_network(ssid, password, 60000);
    memset(password, 0, sizeof(password));
    ESP_LOGI(TAG, "Improv: \"%.32s\": %s", ssid, esp_err_to_name(err));
    if (err == ESP_OK) {
        send_state(IMPROV_STATE_PROVISIONED);
        send_url(IMPROV_CMD_WIFI_SETTINGS);
    } else {
        send_error(IMPROV_ERROR_UNABLE_TO_CONNECT);
        send_state(IMPROV_STATE_READY);
    }
}

static void improv_task(void *arg)
{
    static improv_rpc_t rpc;  /* 257 bytes: not on this task's stack */
    while (xQueueReceive(s_queue, &rpc, portMAX_DELAY) == pdTRUE) {
        switch (rpc.command) {
        case IMPROV_CMD_WIFI_SETTINGS:
            set_network(&rpc);
            break;
        case IMPROV_CMD_GET_STATE:
            send_state(current_state());
            if (current_state() == IMPROV_STATE_PROVISIONED) {
                send_url(IMPROV_CMD_WIFI_SETTINGS);
            }
            break;
        case IMPROV_CMD_GET_INFO:
            send_info();
            break;
        case IMPROV_CMD_GET_NETWORKS:
            send_networks();
            break;
        case BAD_PACKET:
            send_error(IMPROV_ERROR_INVALID_RPC);
            break;
        default:
            send_error(IMPROV_ERROR_UNKNOWN_RPC);
            break;
        }
    }
}

esp_err_t pairble_improv_start(pairble_improv_write_t write, void *ctx, const char *url)
{
    ESP_RETURN_ON_FALSE(write != NULL, ESP_ERR_INVALID_ARG, TAG, "no write function");
    ESP_RETURN_ON_FALSE(s_queue == NULL, ESP_ERR_INVALID_STATE, TAG, "Improv already started");
    s_write = write;
    s_write_ctx = ctx;
    strlcpy(s_url, url != NULL ? url : "", sizeof(s_url));
    s_queue = xQueueCreate(2, sizeof(improv_rpc_t));
    ESP_RETURN_ON_FALSE(s_queue != NULL, ESP_ERR_NO_MEM, TAG, "no memory for Improv");
    /* Commands that wait (a connection attempt, a scan) run here, never on
     * the caller's task. */
    if (xTaskCreate(improv_task, "pairble_improv", 4096, NULL, 4, NULL) != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void pairble_improv_set_firmware(const char *name, const char *version)
{
    strlcpy(s_fw_name, name != NULL ? name : "", sizeof(s_fw_name));
    strlcpy(s_fw_version, version != NULL ? version : "", sizeof(s_fw_version));
}

void pairble_improv_input(const uint8_t *data, size_t len)
{
    static improv_rpc_t rpc;  /* only ever one caller, the port's reader */
    if (s_queue == NULL) {
        return;
    }
    for (size_t i = 0; i < len; i++) {
        improv_event_t e = improv_parse_byte(&s_parser, data[i], &rpc);
        if (e == IMPROV_BAD_PACKET) {
            rpc.command = BAD_PACKET;
            rpc.len = 0;
        }
        if (e != IMPROV_NONE) {
            /* A command arriving while one is running is dropped: Improv
             * clients send one at a time and wait for the answer. */
            xQueueSend(s_queue, &rpc, 0);
        }
    }
}

#if CONFIG_PAIRBLE_IMPROV_UART

static void uart_write(const uint8_t *data, size_t len, void *ctx)
{
    uart_write_bytes((uart_port_t)(intptr_t)ctx, data, len);
}

static void uart_reader(void *arg)
{
    uart_port_t port = (uart_port_t)(intptr_t)arg;
    uint8_t buf[64];
    for (;;) {
        int n = uart_read_bytes(port, buf, sizeof(buf), pdMS_TO_TICKS(50));
        if (n > 0) {
            pairble_improv_input(buf, (size_t)n);
        }
    }
}

esp_err_t pairble_improv_uart_start(void)
{
    uart_port_t port = CONFIG_PAIRBLE_IMPROV_UART_NUM;
    if (!uart_is_driver_installed(port)) {
        /* The console's settings stay; this only adds the receive side. */
        ESP_RETURN_ON_ERROR(uart_driver_install(port, 512, 0, 0, NULL, 0), TAG, "UART %d", port);
    }
    ESP_RETURN_ON_ERROR(pairble_improv_start(uart_write, (void *)(intptr_t)port, CONFIG_PAIRBLE_IMPROV_URL), TAG,
                        "Improv");
    if (xTaskCreate(uart_reader, "pairble_uart", 2560, (void *)(intptr_t)port, 4, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Improv Wi-Fi on UART %d", port);
    return ESP_OK;
}

#endif
