/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "pairble.h"

#define PAIRBLE_TAG "pairble"

/* The outcome of the latest credentials, whichever way they arrived. Kept
 * here rather than read from the manager, whose state is reset after a
 * failure so the person can retry - which would erase the answer before a
 * polling client has seen it. */
typedef enum {
    PAIRBLE_RESULT_IDLE,
    PAIRBLE_RESULT_CONNECTING,
    PAIRBLE_RESULT_FAILED,
    PAIRBLE_RESULT_CONNECTED,
} pairble_result_state_t;

typedef struct {
    pairble_result_state_t state;
    pairble_fail_reason_t reason;
    char ip[16];
} pairble_result_t;

pairble_result_t pairble_current_result(void);

/* Marks an attempt as started before the manager has taken it up. The manager
 * reports CRED_RECV asynchronously, so without this a status poll right after
 * a submission could still read the previous attempt's failure and report it
 * as this one's. */
void pairble_result_set_connecting(void);

/* The page has read a successful result: setup can close. */
void pairble_result_delivered(void);

esp_err_t pairble_softap_start(const char *ssid, const char *password);
void pairble_softap_stop(void);

/* The per-unit proof of possession, in NVS namespace "pairble", key "pop". */
esp_err_t pairble_nvs_get_pop(char *out, size_t len);
esp_err_t pairble_nvs_set_pop(const char *pop);

/* Small values in the same namespace: the power-cycle counter ("cycles") and
 * the fast-reconnect hint ("ap"). A missing key reads as ESP_ERR_NVS_NOT_FOUND. */
esp_err_t pairble_nvs_get_blob(const char *key, void *out, size_t len);
esp_err_t pairble_nvs_set_blob(const char *key, const void *data, size_t len);
esp_err_t pairble_nvs_erase(const char *key);

/* pairble_triggers.c. Counts this boot if it was a power-on and returns true
 * when it completes CONFIG_PAIRBLE_POWER_CYCLE_COUNT quick ones in a row. */
bool pairble_power_cycles_reached(void);
/* Starts watching CONFIG_PAIRBLE_SETUP_BUTTON_GPIO, if there is one. */
esp_err_t pairble_button_start(void);

/* pairble_improv.c: Improv on CONFIG_PAIRBLE_IMPROV_UART_NUM. */
esp_err_t pairble_improv_uart_start(void);
