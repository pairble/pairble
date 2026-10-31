/*
 * Shared between the three parts of the Pico port. SPDX-License-Identifier: Apache-2.0
 *
 *   pairble_pico.cpp        state, Wi-Fi, flash, the public API (loop context)
 *   pairble_pico_ble.cpp    the GATT server on BTstack (Bluetooth context)
 *   pairble_pico_proto.cpp  protocomm: security 1 and the four endpoints
 *
 * The Bluetooth callbacks run in the CYW43 async context: from its interrupt,
 * and also from loop() itself whenever loop() lets go of that context's lock
 * (every pairble_unlock(), WiFi or lwIP call, delay()). Whatever both sides
 * touch is written from loop() only under pairble_lock(), which holds the
 * callbacks off; times they set can be later than a millis() loop() took
 * before, so loop() compares them with pb_since().
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pairble.h"  /* the CONFIG_PAIRBLE_* defaults */

#define PB_ENDPOINTS        5
#define PB_MAX_PAYLOAD      512   /* one request or response */
#define PB_MAX_SCAN         24

typedef struct {
    char ssid[33];
    uint8_t bssid[6];
    uint8_t channel;
    int8_t rssi;
    uint8_t auth;                 /* WifiAuthMode */
} pb_scan_entry_t;

/* What the phone sees of the station, protocomm's WifiStationState. */
typedef enum { PB_STA_CONNECTED = 0, PB_STA_CONNECTING = 1, PB_STA_DISCONNECTED = 2, PB_STA_FAILED = 3 } pb_sta_state_t;
typedef enum { PB_REASON_AUTH = 0, PB_REASON_NOT_FOUND = 1 } pb_sta_reason_t;

typedef struct {
    /* Written by loop() under the lock, read by the Bluetooth side. */
    pb_scan_entry_t scan[PB_MAX_SCAN];
    uint8_t scan_count;
    bool scan_done;               /* there is a list to hand out             */
    pb_sta_state_t sta_state;
    pb_sta_reason_t sta_reason;
    uint8_t attempts_remaining;
    char ip[16];
    char connected_ssid[33];
    uint8_t connected_bssid[6];
    int8_t connected_channel;
    volatile bool result_delivered;        /* a phone has read CONNECTED */
    volatile uint32_t result_delivered_at;
    volatile bool result_over_http;        /* ... from the SoftAP page */
    volatile uint32_t hangup_at;           /* ... over Bluetooth, and then hung up */
    bool result_in_response;               /* the answer just made says CONNECTED */

    /* Written by the Bluetooth side, taken by loop(). */
    volatile bool want_scan;
    volatile bool want_apply;
    volatile bool want_reprov;
    char new_ssid[33];
    char new_pass[65];
} pb_shared_t;

extern pb_shared_t pb_shared;

#ifdef __cplusplus
extern "C" {
#endif

void pairble_lock(void);
void pairble_unlock(void);
uint32_t pairble_random32(void);

/* protocomm (pairble_pico_proto.cpp) */
void pb_proto_init(const char *pop);
void pb_proto_new_keypair(void);   /* loop context: X25519 is slow on an M0+ */
void pb_proto_reset_session(void);
/* Endpoint index: 0 prov-ctrl, 1 prov-scan, 2 prov-session, 3 prov-config,
 * 4 proto-ver - the order, and the UUID offsets 0xFF4F.., of network_provisioning. */
int pb_proto_handle(int endpoint, const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap, size_t *out_len);
extern const char *const pb_endpoint_names[PB_ENDPOINTS];

/* pairble_pico.cpp, for the other ways in (loop context) */
void pb_request_network(const char *ssid, const char *pass);
void pb_request_scan(void);
bool pb_scanning(void);
/* Milliseconds since t, 0 for a t later than now. */
uint32_t pb_since(uint32_t t);

/* SoftAP (pairble_pico_softap.cpp) and Improv (pairble_pico_improv.cpp) */
void pb_softap_start(const char *name, const char *pop);
void pb_softap_stop(void);
void pb_softap_loop(void);
bool pb_softap_has_clients(void);
bool pb_softap_running(void);
void pb_improv_loop(void);

/* Bluetooth (pairble_pico_ble.cpp), called from loop() */
bool pb_ble_start(const char *name, const uint8_t service_uuid_be[16]);
bool pb_ble_buffers(bool on);       /* allocate (true) or free the GATT buffers */
void pb_ble_advertise(bool on);
void pb_ble_disconnect(void);
bool pb_ble_connected(void);
void pb_ble_log(void);

#ifdef __cplusplus
}
#endif
