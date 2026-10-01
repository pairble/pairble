/*
 * pairble - Wi-Fi provisioning for ESP32 over BLE with no app to install
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * A thin layer over espressif/network_provisioning. The layer is thin on
 * purpose; what it adds is the handful of things measured against real
 * hardware that the stock example gets wrong or leaves to chance
 * (measured on hardware):
 *
 *   - the failure reason comes from network_prov_mgr_get_wifi_disconnect_reason(),
 *     never from the CRED_FAIL event data, which on ESP-IDF 5.x carries the
 *     reason of the previous failure;
 *   - the retry budget defaults to two attempts, because five made a wrong
 *     password take 54 seconds to report;
 *   - credentials are never logged, the password least of all;
 *   - after a failure the state machine is reset, so the user can correct the
 *     password in the same session instead of power-cycling the device;
 *   - when the stored network stops working, setup reopens without erasing
 *     it: the old network is retried while
 *     setup is open, and replaced only once the device has joined a new one.
 */

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include "bootloader_random.h"
#include "esp_attr.h"
#include "esp_bt.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "nvs_flash.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "network_provisioning/manager.h"
#include "network_provisioning/scheme_ble.h"
#include "protocomm_ble.h"
#include "pairble_internal.h"
#include "pairble_text.h"
#if CONFIG_PAIRBLE_BLE_IDLE_TIMEOUT_SECONDS > 0
#if __has_include("nimble/nimble/host/include/host/ble_gap.h")
#include "nimble/nimble/host/include/host/ble_gap.h"  /* the Arduino library: NimBLE-Arduino's host */
#else
#include "host/ble_gap.h"
#endif
#endif

static const char *TAG = PAIRBLE_TAG;

static pairble_config_t s_config;
static bool s_initialised;
static char s_setup_url[192];
static char s_wifi_qr[128];
static pairble_result_t s_result;  /* read by the httpd task: take s_lock */
/* Guards s_result, and makes "is setup opening?" a single step for the tasks
 * that can ask at once: the application, the button and recovery timers. */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_provisioning;
static bool s_succeeded;
static char s_device_name[32];
static char s_pop[65];
/* network_prov_scheme_ble_set_service_uuid() keeps the pointer, so this must
 * outlive the call. */
static uint8_t s_service_uuid[16];
static esp_timer_handle_t s_window_timer;
static esp_timer_handle_t s_reboot_timer;
/* After success: closes setup once the phone has the result, instead of after
 * the manager's own CONFIG_NETWORK_PROV_AUTOSTOP_TIMEOUT (30 s by default),
 * which stays as the fallback for a phone that never says. */
static esp_timer_handle_t s_finish_timer;
static network_prov_mgr_config_t s_mgr_cfg;
static bool s_mgr_ready;       /* network_prov_mgr_init() done, not yet deinit */
static bool s_from_app;        /* the attempt running came from pairble_set_network() */
static bool s_bt_released;     /* BLE memory gone for good: reopening needs a reset */
static volatile bool s_opening;
static volatile bool s_closing;  /* stop requested, NETWORK_PROV_END not yet seen */

/* ---- the stored network --------------------------------------------------- */
static bool s_online;
typedef enum {
    SETUP_FIRST,   /* nothing stored yet                                          */
    SETUP_AUTO,    /* offline too long: closes as soon as the stored network is back */
    SETUP_MANUAL,  /* asked for (button, API, power cycles): stays online, and open
                      until a phone is done or the window ends                    */
} setup_mode_t;
static setup_mode_t s_mode;
static bool s_recovery;        /* setup reopened on a provisioned device       */
static bool s_creds_received;  /* a phone sent a network in this session       */
static wifi_config_t s_stored; /* the network kept while setup is reopened     */
static pairble_fail_reason_t s_offline_reason;
static uint32_t s_backoff_s;
static bool s_fast_hint;       /* this attempt aims at the remembered AP       */
static int s_ap_clients;
static uint8_t s_last_disconnect;  /* raw reason of the latest failed attempt   */
static bool s_self_disconnect;     /* the next STA_DISCONNECTED is pairble's own */
static esp_timer_handle_t s_retry_timer;    /* reconnect, or retry s_stored during setup */
static esp_timer_handle_t s_recovery_timer; /* offline long enough: reopen setup        */

#define MAX_BACKOFF_S 30
#define BOOT_INTO_SETUP 0x5e7b0070u  /* | setup_mode_t */
/* Survives esp_restart(), not a power cut: the way pairble_open_setup() asks
 * the next boot for setup when the BLE memory is gone. */
static RTC_NOINIT_ATTR uint32_t s_boot_into_setup;
/* When the device went offline, by the system clock, which keeps running
 * through deep sleep; 0 = online, or not known to be offline. */
static RTC_DATA_ATTR int64_t s_offline_since_us;
/* s_offline_since_us as it was before the latest disconnect: what it goes
 * back to if that disconnect turns out to be the application stopping Wi-Fi. */
static int64_t s_offline_since_before;
static bool s_client_connected;
static bool s_softap_down;  /* wanted, but its HTTP server did not start */

/* ---- idle BLE client guard -------------------------------------------
 * A connected phone stops the device advertising, so a client that never
 * lets go makes the device invisible to everyone else (22 minutes, measured).
 * This drops such a client. See PAIRBLE_BLE_IDLE_TIMEOUT_SECONDS. */
#if CONFIG_PAIRBLE_BLE_IDLE_TIMEOUT_SECONDS > 0
static esp_timer_handle_t s_idle_timer;
static uint16_t s_conn_handle;

static void idle_arm(void)
{
    if (s_idle_timer == NULL || !s_client_connected) {
        return;
    }
    esp_timer_stop(s_idle_timer);
    esp_timer_start_once(s_idle_timer, (uint64_t)CONFIG_PAIRBLE_BLE_IDLE_TIMEOUT_SECONDS * 1000 * 1000);
}

static void idle_disarm(void)
{
    if (s_idle_timer != NULL) {
        esp_timer_stop(s_idle_timer);
    }
}

static void idle_expired_cb(void *arg)
{
    if (!s_client_connected) {
        return;
    }
    ESP_LOGW(TAG, "phone idle for %d s, disconnecting it so others can find the device",
             CONFIG_PAIRBLE_BLE_IDLE_TIMEOUT_SECONDS);
    /* The same call protocomm uses to drop a client; protocomm then resumes
     * advertising on the disconnect event. */
    ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
}

#else
static inline void idle_arm(void) {}
static inline void idle_disarm(void) {}
#endif

static void phone_arrived(void);
static void finish_soon(uint32_t ms);

static void on_ble_transport_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == PROTOCOMM_TRANSPORT_BLE_CONNECTED && data != NULL) {
#if CONFIG_PAIRBLE_BLE_IDLE_TIMEOUT_SECONDS > 0
        s_conn_handle = ((const protocomm_ble_event_t *)data)->conn_handle;
#endif
        s_client_connected = true;
        idle_arm();
        phone_arrived();
    } else if (id == PROTOCOMM_TRANSPORT_BLE_DISCONNECTED) {
        /* protocomm posts this with and without event data. */
        s_client_connected = false;
        idle_disarm();
        /* The app and the web page hang up once they have read the result.
         * Only then: after a lost link the phone may still come back for it,
         * and the manager's own timeout closes setup. NimBLE reports the HCI
         * reason with BLE_HS_ERR_HCI_BASE (0x200) added. */
        uint16_t reason = data != NULL ? ((const protocomm_ble_event_t *)data)->disconnect_reason : 0;
        if (reason == 0x13 || reason == 0x213) {  /* remote user terminated */
            finish_soon(300);
        }
    }
}

static void emit(pairble_event_t event, pairble_fail_reason_t reason)
{
    if (s_config.event_cb != NULL) {
        s_config.event_cb(event, reason, s_config.user_ctx);
    }
}

static void reboot_cb(void *arg)
{
    ESP_LOGI(TAG, "restarting with the stored credentials");
    esp_restart();
}

static void finish_cb(void *arg)
{
    if (s_succeeded && s_provisioning && !s_closing) {
        ESP_LOGI(TAG, "the phone has the result, closing setup");
        s_closing = true;
        network_prov_mgr_stop_provisioning();
    }
}

/* Called once the phone has been told that it worked. */
static void finish_soon(uint32_t ms)
{
    if (s_succeeded && s_finish_timer != NULL) {
        esp_timer_stop(s_finish_timer);
        esp_timer_start_once(s_finish_timer, (uint64_t)ms * 1000);
    }
}

void pairble_result_delivered(void)
{
    /* Time for the response to reach the page before the network goes. Not
     * restarted by later polls, or two pages polling would keep setup open. */
    if (s_finish_timer != NULL && !esp_timer_is_active(s_finish_timer)) {
        finish_soon(1500);
    }
}

static void window_expired_cb(void *arg)
{
    ESP_LOGW(TAG, "setup window elapsed, closing");
    s_closing = true;
    network_prov_mgr_stop_provisioning();
}

static int64_t now_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

/* ---- Bluetooth memory ------------------------------------------------------ */

/* In place of the stock FREE_BTDM handler, which releases BLE's memory too
 * and so rules out reopening setup, and takes Bluetooth away from an
 * application that uses it. See CONFIG_PAIRBLE_BT_MEMORY. */
static void bt_memory_cb(void *user_data, network_prov_cb_event_t event, void *event_data)
{
#if CONFIG_BT_CONTROLLER_ENABLED
#if CONFIG_PAIRBLE_BT_MEMORY_RELEASE_CLASSIC || CONFIG_PAIRBLE_BT_MEMORY_RELEASE_ALL
    static bool classic_released;
    if (event == NETWORK_PROV_INIT && !classic_released) {
        classic_released = esp_bt_mem_release(ESP_BT_MODE_CLASSIC_BT) == ESP_OK;
    }
#endif
#if CONFIG_PAIRBLE_BT_MEMORY_RELEASE_ALL
    if (event == NETWORK_PROV_DEINIT && !s_bt_released) {
        s_bt_released = esp_bt_mem_release(ESP_BT_MODE_BTDM) == ESP_OK;
        ESP_LOGI(TAG, "Bluetooth memory released");
    }
#endif
#endif
}

/* ---- the stored network ---------------------------------------------------- */

/* Puts `cfg` in the station config. RAM only, unless `to_flash`: the hints
 * pairble adds (scan method, remembered AP) are per boot, and must not end up
 * in what the next boot reads as the stored network. */
static esp_err_t set_station(const wifi_config_t *cfg, bool to_flash)
{
    if (!to_flash) {
        esp_wifi_set_storage(WIFI_STORAGE_RAM);
    }
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, (wifi_config_t *)cfg);
    esp_wifi_set_storage(WIFI_STORAGE_FLASH);
    return err;
}

static void result_set(pairble_result_state_t state, pairble_fail_reason_t reason)
{
    portENTER_CRITICAL(&s_lock);
    s_result.state = state;
    s_result.reason = reason;
    if (state != PAIRBLE_RESULT_CONNECTED) {
        s_result.ip[0] = '\0';
    }
    portEXIT_CRITICAL(&s_lock);
}

typedef struct {
    uint8_t bssid[6];
    uint8_t channel;
} ap_hint_t;

/* Scan every channel and take the strongest access point, rather than the
 * first one that answers: in a mesh the first is often the far one. */
static void prepare_station(void)
{
    wifi_config_t cfg;
    if (esp_wifi_get_config(WIFI_IF_STA, &cfg) != ESP_OK || cfg.sta.ssid[0] == '\0') {
        return;
    }
    cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
#if CONFIG_PAIRBLE_FAST_RECONNECT
    ap_hint_t hint;
    if (pairble_nvs_get_blob("ap", &hint, sizeof(hint)) == ESP_OK && hint.channel != 0) {
        memcpy(cfg.sta.bssid, hint.bssid, sizeof(hint.bssid));
        cfg.sta.bssid_set = true;
        cfg.sta.channel = hint.channel;
        s_fast_hint = true;
    }
#endif
    set_station(&cfg, false);
}

static void forget_fast_hint(void)
{
    wifi_config_t cfg;
    s_fast_hint = false;
    pairble_nvs_erase("ap");
    if (esp_wifi_get_config(WIFI_IF_STA, &cfg) == ESP_OK) {
        cfg.sta.bssid_set = false;
        cfg.sta.channel = 0;
        set_station(&cfg, false);
    }
    ESP_LOGI(TAG, "the remembered access point did not answer; scanning instead");
}

static void remember_fast_hint(void)
{
#if CONFIG_PAIRBLE_FAST_RECONNECT
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        return;
    }
    ap_hint_t now = { .channel = ap.primary }, before;
    memcpy(now.bssid, ap.bssid, sizeof(now.bssid));
    if (pairble_nvs_get_blob("ap", &before, sizeof(before)) != ESP_OK ||
        memcmp(&now, &before, sizeof(now)) != 0) {
        pairble_nvs_set_blob("ap", &now, sizeof(now));  /* only when it changed: flash wear */
    }
#endif
}

static pairble_fail_reason_t classify(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_MIC_FAILURE:
        return PAIRBLE_FAIL_WRONG_PASSWORD;
    case WIFI_REASON_NO_AP_FOUND:
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 2, 0)
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
#endif
        return PAIRBLE_FAIL_AP_NOT_FOUND;
    default:
        return PAIRBLE_FAIL_CONNECTION_LOST;
    }
}

static const char *reason_text(pairble_fail_reason_t reason)
{
    switch (reason) {
    case PAIRBLE_FAIL_WRONG_PASSWORD: return "wrong password";
    case PAIRBLE_FAIL_AP_NOT_FOUND:   return "network not found";
    default:                          return "connection lost";
    }
}

/* Starts counting towards reopening setup, unless it is counting already.
 * `at_least_s` gives a device waking from deep sleep a moment to connect
 * before setup opens on the strength of time spent asleep. */
static void arm_recovery(uint32_t at_least_s)
{
    if (s_config.recovery_after_seconds == 0 || s_provisioning || s_recovery_timer == NULL ||
        esp_timer_is_active(s_recovery_timer)) {
        return;
    }
    int64_t now = now_us();
    if (s_offline_since_us == 0 || s_offline_since_us > now) {  /* > now: the clock was set back */
        s_offline_since_us = now;
    }
    int64_t left = (int64_t)s_config.recovery_after_seconds * 1000000 - (now - s_offline_since_us);
    if (left < (int64_t)at_least_s * 1000000) {
        left = (int64_t)at_least_s * 1000000;
    }
    esp_timer_start_once(s_recovery_timer, (uint64_t)left);
}

static void schedule_reconnect(void)
{
    uint32_t delay_s = s_backoff_s;
    s_backoff_s = s_backoff_s == 0 ? 1 : (s_backoff_s * 2 > MAX_BACKOFF_S ? MAX_BACKOFF_S : s_backoff_s * 2);
    if (delay_s == 0) {
        esp_wifi_connect();
    } else {
        esp_timer_stop(s_retry_timer);
        esp_timer_start_once(s_retry_timer, (uint64_t)delay_s * 1000000);
    }
}

static void went_online(const ip_event_got_ip_t *got)
{
    char ip[16];
    snprintf(ip, sizeof(ip), IPSTR, IP2STR(&got->ip_info.ip));
    portENTER_CRITICAL(&s_lock);
    memcpy(s_result.ip, ip, sizeof(ip));
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "online at " IPSTR, IP2STR(&got->ip_info.ip));
    s_online = true;
    s_offline_since_us = 0;
    s_offline_since_before = 0;
    s_offline_reason = PAIRBLE_FAIL_NONE;
    s_backoff_s = 0;
    if (s_recovery_timer != NULL) {
        esp_timer_stop(s_recovery_timer);
    }
    remember_fast_hint();
    if (s_fast_hint) {
        /* It worked, and is kept in NVS for the next boot. Unpinned for this
         * one, so a later reconnect is not tied to an access point that may
         * have moved - which would fail, and then forget the hint. */
        wifi_config_t cfg;
        if (esp_wifi_get_config(WIFI_IF_STA, &cfg) == ESP_OK) {
            cfg.sta.bssid_set = false;
            cfg.sta.channel = 0;
            set_station(&cfg, false);
        }
        s_fast_hint = false;
    }
    emit(PAIRBLE_EVENT_ONLINE, PAIRBLE_FAIL_NONE);
}

static void went_offline(uint8_t raw_reason)
{
    if (s_self_disconnect) {
        /* pairble dropped the connection to change the config: not news, and
         * no reason to back off. Anything else - the application's own
         * esp_wifi_disconnect() included - goes through backoff below, which
         * is what stops a tight loop against an AP that keeps deauthing. */
        s_self_disconnect = false;
        s_online = false;
        esp_wifi_connect();
        return;
    }
    s_offline_since_before = s_offline_since_us;
    if (raw_reason == WIFI_REASON_ASSOC_LEAVE) {
        /* The station left on its own side - the application disconnected,
         * or is stopping Wi-Fi before sleep (STA_STOP follows and undoes
         * this). Not reported, and the remembered AP stays: only a failed
         * reconnect says anything about the network. Reconnect after a
         * second, by which time a stop has cancelled it. */
        s_online = false;
        arm_recovery(0);
        esp_timer_stop(s_retry_timer);
        esp_timer_start_once(s_retry_timer, 1000 * 1000);
        return;
    }
    pairble_fail_reason_t reason = classify(raw_reason);
    s_online = false;
    if (s_fast_hint) {
        forget_fast_hint();
        s_backoff_s = 0;  /* straight into a proper scan */
    }
    if (reason != s_offline_reason) {
        ESP_LOGW(TAG, "offline: %s (reason %d), retrying", reason_text(reason), raw_reason);
        s_offline_reason = reason;
        emit(PAIRBLE_EVENT_OFFLINE, reason);
    }
    arm_recovery(0);
    schedule_reconnect();
}

/* With setup reopened, a phone and a connection attempt compete for the one
 * radio: the phone's network scan fails while the station is connecting. So
 * the attempt yields to the phone. */
static bool phone_present(void)
{
    return s_client_connected || s_ap_clients > 0;
}

static void phone_arrived(void)
{
    /* Also when connected: the phone's credentials cannot be applied over a
     * live connection, and a scan is fine without one. */
    if (s_provisioning && s_recovery && !s_creds_received) {
        esp_wifi_disconnect();
    }
}

static void retry_cb(void *arg)
{
    if (!s_provisioning) {
        esp_wifi_connect();  /* backoff elapsed */
        return;
    }
    if (!s_recovery || s_creds_received || s_closing) {
        return;
    }
    if (!phone_present() && !s_online) {
        ESP_LOGI(TAG, "trying the stored network \"%.32s\" again", (const char *)s_stored.sta.ssid);
        set_station(&s_stored, false);
        esp_wifi_connect();
    }
    esp_timer_start_once(s_retry_timer, (uint64_t)CONFIG_PAIRBLE_RECOVERY_RETRY_SECONDS * 1000000);
}

static esp_err_t request_setup(setup_mode_t mode);

static void recovery_cb(void *arg)
{
    if (s_online || s_provisioning) {
        return;
    }
    ESP_LOGW(TAG, "offline for %lu s: reopening setup, keeping the stored network",
             (unsigned long)s_config.recovery_after_seconds);
    request_setup(SETUP_AUTO);
}

/* The manager writes new credentials to flash the moment they arrive, and
 * wipes them there after a failure. While setup is reopened, both would lose
 * the network that was working until recently; restore_stored() puts it back,
 * and a copy in NVS, taken before any phone can send anything, covers a power
 * cut in between. It is dropped when setup closes or new credentials work. */
static void keep_stored_while_trying(void)
{
    pairble_nvs_set_blob("prev", &s_stored.sta, sizeof(s_stored.sta));
}

/* Returns true if it had to drop a connection attempt to do it, in which case
 * a STA_DISCONNECTED follows. */
static bool restore_stored(void)
{
    if (set_station(&s_stored, true) == ESP_OK) {
        return false;
    }
    /* "sta is connecting": one of our retries is in flight. */
    s_self_disconnect = true;
    esp_wifi_disconnect();
    set_station(&s_stored, true);
    return true;
}

/* At boot: a power cut while new credentials were being tried left them in
 * flash, untested, and the working ones only in the backup. */
static void recover_interrupted_attempt(void)
{
    wifi_config_t cfg = { 0 }, now;
    if (pairble_nvs_get_blob("prev", &cfg.sta, sizeof(cfg.sta)) == ESP_OK &&
        esp_wifi_get_config(WIFI_IF_STA, &now) == ESP_OK &&
        (memcmp(cfg.sta.ssid, now.sta.ssid, sizeof(now.sta.ssid)) != 0 ||
         memcmp(cfg.sta.password, now.sta.password, sizeof(now.sta.password)) != 0)) {
        ESP_LOGW(TAG, "setup was interrupted while trying a new network; keeping \"%.32s\"",
                 (const char *)cfg.sta.ssid);
        set_station(&cfg, true);
    }
    pairble_nvs_erase("prev");
}

/* ---- events ----------------------------------------------------------------- */

static void on_prov_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    switch (id) {
    case NETWORK_PROV_START:
        /* PAIRBLE_EVENT_STARTED comes from open_setup(), once the SoftAP and
         * pairble_wifi_qr() are ready too. */
        ESP_LOGI(TAG, "advertising as %s", s_device_name);
        break;

    case NETWORK_PROV_WIFI_CRED_RECV: {
        /* The SSID is fine to log. The passphrase never is: it would end up on
         * the serial console, in CI output and in support tickets. */
        const wifi_sta_config_t *sta = (const wifi_sta_config_t *)data;
        ESP_LOGI(TAG, "received credentials for \"%.32s\"", (const char *)sta->ssid);
        result_set(PAIRBLE_RESULT_CONNECTING, PAIRBLE_FAIL_NONE);
        s_creds_received = true;
        if (s_softap_down) {
            /* The manager has just put Wi-Fi back into AP+STA for these
             * credentials, which would bring up the access point with no
             * setup page behind it. It connects a second from now. */
            esp_wifi_set_mode(WIFI_MODE_STA);
        }
        s_last_disconnect = 0;
        idle_disarm();  /* the device is busy trying them, not idle */
        emit(PAIRBLE_EVENT_CREDENTIALS_RECEIVED, PAIRBLE_FAIL_NONE);
        break;
    }

    case NETWORK_PROV_WIFI_CRED_FAIL: {
        s_from_app = false;
        /* Deliberately not the manager's reason. The event data is a
         * snapshot taken before the manager updates it (on ESP-IDF 5.x's
         * built-in component), and even the getter only knows five raw
         * reasons: for any other - 205 at the edge of range, 210 for an AP
         * whose security the device will not use - it keeps the previous
         * failure's, "wrong password" on a first attempt. pairble records
         * the raw reason itself (on_wifi_event) and classifies all of them. */
        pairble_fail_reason_t reason = PAIRBLE_FAIL_CONNECTION_LOST;
        network_prov_wifi_sta_fail_reason_t raw;
        if (s_last_disconnect != 0) {
            reason = classify(s_last_disconnect);
        } else if (network_prov_mgr_get_wifi_disconnect_reason(&raw) == ESP_OK) {
            reason = raw == NETWORK_PROV_WIFI_STA_AUTH_ERROR ? PAIRBLE_FAIL_WRONG_PASSWORD
                                                             : PAIRBLE_FAIL_AP_NOT_FOUND;
        }
        ESP_LOGW(TAG, "credentials failed: %s (reason %d)", reason_text(reason), s_last_disconnect);
        result_set(PAIRBLE_RESULT_FAILED, reason);
        emit(PAIRBLE_EVENT_FAILED, reason);
        /* Let the user correct it without power-cycling the device. */
        network_prov_mgr_reset_wifi_sm_state_on_failure();
        if (s_recovery) {
            restore_stored();
            s_creds_received = false;  /* back to retrying the stored network */
            esp_timer_stop(s_retry_timer);
            esp_timer_start_once(s_retry_timer, (uint64_t)CONFIG_PAIRBLE_RECOVERY_RETRY_SECONDS * 1000000);
        } else {
            s_creds_received = false;  /* first setup: the next network may come from anywhere */
        }
        idle_arm();  /* the person may now walk away mid-retry */
        break;
    }

    case NETWORK_PROV_WIFI_CRED_SUCCESS:
        ESP_LOGI(TAG, "joined the network");
        portENTER_CRITICAL(&s_lock);
        s_result.state = PAIRBLE_RESULT_CONNECTED;  /* the IP is already in, from went_online() */
        portEXIT_CRITICAL(&s_lock);
        idle_disarm();
        s_succeeded = true;
        pairble_nvs_erase("prev");
        emit(PAIRBLE_EVENT_SUCCESS, PAIRBLE_FAIL_NONE);
        if (s_from_app) {
            /* No phone to wait for: the application has its answer. */
            s_from_app = false;
            finish_soon(500);
        }
        break;

    case NETWORK_PROV_END: {
        /* The manager keeps BLE up for a while after success so the phone
         * can read the final status; this is where it lets go. */
        bool was_recovery = s_recovery;
        idle_disarm();
        pairble_softap_stop();
        esp_timer_stop(s_finish_timer);
        esp_timer_stop(s_window_timer);
        esp_timer_stop(s_retry_timer);
        network_prov_mgr_deinit();
        s_mgr_ready = false;
        s_ap_clients = 0;
        s_client_connected = false;
        s_from_app = false;
        s_recovery = false;
        s_closing = false;
        pairble_nvs_erase("prev");
        /* Last: until here, a request to open setup must wait (request_setup()
         * treats s_provisioning as "busy"), or it would find a manager that
         * is being torn down. */
        portENTER_CRITICAL(&s_lock);
        s_provisioning = false;
        portEXIT_CRITICAL(&s_lock);
        emit(PAIRBLE_EVENT_STOPPED, PAIRBLE_FAIL_NONE);
        if (s_succeeded) {
            if (s_config.reboot_after_success) {
                esp_timer_start_once(s_reboot_timer, 1000 * 1000);
            }
        } else if (was_recovery && !s_online) {
            /* Closed with nothing new: back to the stored network, and to
             * counting towards the next time setup opens. */
            bool dropped = restore_stored();
            prepare_station();
            s_offline_since_us = now_us();
            s_offline_since_before = s_offline_since_us;
            s_backoff_s = 0;
            arm_recovery(0);
            if (!dropped) {
                esp_wifi_connect();  /* else the STA_DISCONNECTED that follows does */
            }
        }
        break;
    }

    default:
        break;
    }
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        s_ap_clients++;
        phone_arrived();
        return;
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        s_ap_clients = s_ap_clients > 0 ? s_ap_clients - 1 : 0;
        return;
    }
    if (s_provisioning || s_opening) {
        /* The manager drives the station, except when the stored network
         * comes back while setup is reopened: then setup has no job left. */
        if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
            s_self_disconnect = false;
            s_last_disconnect = ((const wifi_event_sta_disconnected_t *)data)->reason;
            if (s_online) {
                /* A manually opened setup lets go of the stored network when
                 * a phone arrives; the application should hear of it. */
                s_online = false;
                s_offline_reason = PAIRBLE_FAIL_CONNECTION_LOST;
                emit(PAIRBLE_EVENT_OFFLINE, PAIRBLE_FAIL_CONNECTION_LOST);
            }
        } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
            went_online((const ip_event_got_ip_t *)data);
            /* s_provisioning, not just s_opening: before the manager has
             * started there is nothing to stop. A connection that lands in
             * that gap is dropped by starting setup and comes back on the
             * next retry, which then closes setup. */
            if (s_provisioning && s_mode == SETUP_AUTO && !s_creds_received) {
                ESP_LOGI(TAG, "the stored network is back, closing setup");
                s_closing = true;
                network_prov_mgr_stop_provisioning();
            }
        }
        return;
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_STOP) {
        /* The application switched Wi-Fi off - before deep sleep, typically.
         * The disconnect that came with it is not an outage: a device that
         * was online until now must not wake up counting towards reopening
         * setup. One that was already offline keeps counting. */
        esp_timer_stop(s_retry_timer);
        esp_timer_stop(s_recovery_timer);
        s_offline_since_us = s_offline_since_before;
        if (s_offline_since_us == 0) {
            s_offline_reason = PAIRBLE_FAIL_NONE;
        }
        s_backoff_s = 0;
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        went_offline(((const wifi_event_sta_disconnected_t *)data)->reason);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        went_online((const ip_event_got_ip_t *)data);
    }
}

/* ---- proof of possession ------------------------------------------------ */

#if CONFIG_PAIRBLE_POP_GENERATE
/* Crockford base32 without I, L, O and U - the same alphabet @pairble/cli uses,
 * so a generated code survives being read off a display and typed in.
 *
 * Twelve characters, 60 bits. Guessing online costs a BLE handshake each, but
 * two offline attacks exist: a rogue device advertising this name can test
 * guesses against what a phone sends it (Security1 verifies the phone
 * first), and the SoftAP's WPA2 handshake can be captured and cracked. 60
 * bits keeps both out of reach of anything short of a large GPU farm. */
static void generate_pop(char *out, size_t len)
{
    static const char alphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
    uint8_t random[12];
    /* At this point in startup the radio is not running (pairble_start() is
     * documented to come before the application starts Wi-Fi or BLE), and
     * without it the hardware RNG is only pseudo-random. This switches on the
     * SAR ADC noise source for the moment it takes (see bootloader_random.h). */
    bootloader_random_enable();
    esp_fill_random(random, sizeof(random));
    bootloader_random_disable();
    size_t n = sizeof(random) < len - 1 ? sizeof(random) : len - 1;
    for (size_t i = 0; i < n; i++) {
        out[i] = alphabet[random[i] & 31];
    }
    out[n] = '\0';
}
#endif

static esp_err_t resolve_pop(const char *given)
{
    esp_err_t err;
    if (given != NULL) {
        strlcpy(s_pop, given, sizeof(s_pop));
    } else if ((err = pairble_nvs_get_pop(s_pop, sizeof(s_pop))) != ESP_OK) {
        /* Only a missing one is replaced. Anything else - a read error, a
         * value too long - would silently swap the code on a printed label
         * for one nobody knows. */
        ESP_RETURN_ON_FALSE(err == ESP_ERR_NVS_NOT_FOUND, err, TAG, "cannot read the PoP from NVS: %s",
                            esp_err_to_name(err));
#if CONFIG_PAIRBLE_POP_GENERATE
        generate_pop(s_pop, sizeof(s_pop));
        ESP_RETURN_ON_ERROR(pairble_nvs_set_pop(s_pop), TAG, "could not store the generated PoP");
        ESP_LOGI(TAG, "generated a proof of possession for this unit");
#else
        ESP_LOGE(TAG, "no proof of possession in NVS; provision one with @pairble/cli");
        return ESP_ERR_NOT_FOUND;
#endif
    }
    ESP_RETURN_ON_FALSE(pairble_valid_pop(s_pop), ESP_ERR_INVALID_ARG, TAG,
                        "PoP must be 8-63 characters of A-Z, a-z, 0-9 and ._~-");
    return ESP_OK;
}

static esp_err_t resolve_name(const char *given, const char *prefix)
{
    if (given != NULL) {
        strlcpy(s_device_name, given, sizeof(s_device_name));
    } else {
        if (prefix == NULL) {
            prefix = CONFIG_PAIRBLE_NAME_PREFIX;
        }
        ESP_RETURN_ON_FALSE(strlen(prefix) <= PAIRBLE_NAME_MAX_LEN - 6, ESP_ERR_INVALID_ARG, TAG,
                            "name prefix is longer than %d characters", PAIRBLE_NAME_MAX_LEN - 6);
        uint8_t mac[6];
        ESP_RETURN_ON_ERROR(esp_read_mac(mac, ESP_MAC_WIFI_STA), TAG, "no MAC address");
        snprintf(s_device_name, sizeof(s_device_name), "%s%02X%02X%02X", prefix, mac[3], mac[4], mac[5]);
    }
    ESP_RETURN_ON_FALSE(pairble_valid_name(s_device_name, PAIRBLE_NAME_MAX_LEN), ESP_ERR_INVALID_ARG, TAG,
                        "name must be 1-%d characters of A-Z, a-z, 0-9, '-' and '_'", PAIRBLE_NAME_MAX_LEN);
    return ESP_OK;
}

/* ---- the plumbing an application may or may not have set up already ----- */

/* For a platform layer with its own idea of Wi-Fi - the Arduino library: it
 * initialises Wi-Fi here, its way, and leaves it stopped for pairble to start.
 * Nothing in ESP-IDF. */
__attribute__((weak)) void pairble_platform_wifi_init(void)
{
}

static esp_err_t ensure_system(void)
{
    /* Returns ESP_OK if the application initialised NVS already. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* The partition is unusable as it stands; this is the recovery every
         * ESP-IDF example performs, and the one an application would. */
        /* Which takes a PoP flashed by @pairble/cli with it: loud, because the
         * printed label stops working (or a new one is generated). */
        ESP_LOGE(TAG, "NVS unusable (%s), erasing it - including the proof of possession",
                 esp_err_to_name(err));
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "NVS erase failed");
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "NVS init failed");

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init failed");  /* idempotent */

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {  /* INVALID_STATE: exists already */
        return err;
    }

    pairble_platform_wifi_init();
    if (esp_netif_get_handle_from_ifkey("WIFI_STA_DEF") == NULL) {
        esp_netif_create_default_wifi_sta();
    }
    wifi_mode_t mode;
    if (esp_wifi_get_mode(&mode) == ESP_ERR_WIFI_NOT_INIT) {
        wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_RETURN_ON_ERROR(esp_wifi_init(&wifi_cfg), TAG, "Wi-Fi init failed");
    }
    return ESP_OK;
}

/* ---- opening setup ------------------------------------------------------------ */

/* The station config cannot change while a connection attempt is in flight
 * ("sta is connecting"), and changing it is the first thing the manager does
 * when setup starts - so a reopen that lands mid-attempt would fail. */
static void quiet_station(void)
{
    wifi_config_t cfg;
    if (esp_wifi_get_config(WIFI_IF_STA, &cfg) != ESP_OK) {
        return;
    }
    esp_wifi_disconnect();
    for (int waited = 0; waited < 1000; waited += 20) {
        if (set_station(&cfg, false) != ESP_ERR_WIFI_STATE) {
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static esp_err_t open_setup(setup_mode_t mode)
{
    bool recovery = mode != SETUP_FIRST;  /* a stored network, kept and retried */
    if (mode == SETUP_AUTO && s_online) {
        return ESP_OK;  /* the stored network answered while this was on its way */
    }
    if (!s_mgr_ready) {
        ESP_RETURN_ON_ERROR(network_prov_mgr_init(s_mgr_cfg), TAG, "provisioning manager init failed");
        s_mgr_ready = true;
    }
    esp_timer_stop(s_retry_timer);
    esp_timer_stop(s_recovery_timer);
    if (recovery) {
        /* Read before the manager empties the station config in RAM; the
         * copy in flash stays until new credentials arrive. */
        esp_wifi_get_config(WIFI_IF_STA, &s_stored);
        s_stored.sta.bssid_set = false;
        s_stored.sta.channel = 0;
        keep_stored_while_trying();
    }
    s_mode = mode;
    s_recovery = recovery;
    s_creds_received = false;
    s_succeeded = false;
    s_closing = false;
    s_last_disconnect = 0;
    s_softap_down = false;
    result_set(PAIRBLE_RESULT_IDLE, PAIRBLE_FAIL_NONE);
    portENTER_CRITICAL(&s_lock);
    s_provisioning = true;  /* before the disconnect the manager is about to cause */
    portEXIT_CRITICAL(&s_lock);
    if (s_config.softap) {
        /* Ready before STARTED, which a device with a display may draw from. */
        snprintf(s_wifi_qr, sizeof(s_wifi_qr), "WIFI:S:%s;T:WPA;P:%s;;", s_device_name, s_pop);
    }

    quiet_station();
    ESP_ERROR_CHECK(network_prov_scheme_ble_set_service_uuid(s_service_uuid));
    esp_err_t err = network_prov_mgr_start_provisioning(NETWORK_PROV_SECURITY_1, (const void *)s_pop,
                                                        s_device_name, NULL);
    if (err != ESP_OK) {
        s_provisioning = false;
        s_recovery = false;
        if (recovery) {
            /* Starting setup disconnected the station before it failed (out
             * of memory for BLE, typically). Back to the stored network, or
             * the device would stay off it with nothing left to reconnect. */
            pairble_nvs_erase("prev");
            set_station(&s_stored, false);
            prepare_station();
            s_online = false;
            s_backoff_s = 0;
            s_offline_since_us = now_us();
            s_offline_since_before = s_offline_since_us;
            arm_recovery(0);
            esp_wifi_connect();
        }
        return err;
    }
#if CONFIG_PAIRBLE_LOG_SETUP_URL
    ESP_LOGI(TAG, "setup: %s", s_setup_url);
#else
    ESP_LOGI(TAG, "setup open as %s (URL not logged: CONFIG_PAIRBLE_LOG_SETUP_URL)", s_device_name);
#endif
    if (s_config.softap) {
        /* The manager has just started Wi-Fi in STA mode; add the AP to it. */
        err = pairble_softap_start(s_device_name, s_pop);
        s_softap_down = err != ESP_OK;
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "SoftAP fallback unavailable, BLE only: %s", esp_err_to_name(err));
            s_wifi_qr[0] = '\0';
        } else {
#if CONFIG_PAIRBLE_LOG_SETUP_URL
            ESP_LOGI(TAG, "wifi qr: %s", s_wifi_qr);
#endif
        }
    }
    uint32_t window = recovery ? s_config.recovery_window_seconds : s_config.window_seconds;
    if (window > 0) {
        esp_timer_start_once(s_window_timer, (uint64_t)window * 1000 * 1000);
    }
    if (mode == SETUP_AUTO) {
        ESP_LOGI(TAG, "setup reopened; still trying \"%.32s\" every %d s",
                 (const char *)s_stored.sta.ssid, CONFIG_PAIRBLE_RECOVERY_RETRY_SECONDS);
        esp_timer_start_once(s_retry_timer, (uint64_t)CONFIG_PAIRBLE_RECOVERY_RETRY_SECONDS * 1000000);
    } else if (mode == SETUP_MANUAL) {
        /* Starting setup dropped the connection; take it straight back. It
         * lasts until a phone arrives. */
        ESP_LOGI(TAG, "setup opened; staying on \"%.32s\" until a phone connects",
                 (const char *)s_stored.sta.ssid);
        esp_timer_start_once(s_retry_timer, 100 * 1000);
    }
    emit(PAIRBLE_EVENT_STARTED, PAIRBLE_FAIL_NONE);
    return ESP_OK;
}

/* ---- public API ---------------------------------------------------------- */

esp_err_t pairble_start(const pairble_config_t *config)
{
    ESP_RETURN_ON_FALSE(!s_initialised, ESP_ERR_INVALID_STATE, TAG, "pairble_start() already called");

    const pairble_config_t defaults = PAIRBLE_CONFIG_DEFAULT();
    s_config = config != NULL ? *config : defaults;
    if (s_config.recovery_window_seconds == 0) {  /* a zeroed struct: 0 would mean "closed" */
        s_config.recovery_window_seconds = CONFIG_PAIRBLE_RECOVERY_WINDOW_SECONDS;
    }

    ESP_RETURN_ON_ERROR(ensure_system(), TAG, "system init failed");
    ESP_RETURN_ON_ERROR(resolve_pop(s_config.pop), TAG, "no usable PoP");
    ESP_RETURN_ON_ERROR(resolve_name(s_config.name, s_config.name_prefix), TAG, "no usable name");
    ESP_RETURN_ON_FALSE(pairble_parse_uuid(CONFIG_PAIRBLE_SERVICE_UUID, s_service_uuid), ESP_ERR_INVALID_ARG, TAG,
                        "CONFIG_PAIRBLE_SERVICE_UUID is not a valid 128-bit UUID");

    /* Fragment, not query: browsers never send the fragment to the server, so
     * the proof of possession stays out of the host's access logs. Verified on
     * hardware that an App Clip receives it. */
    int n = snprintf(s_setup_url, sizeof(s_setup_url), "%s#name=%s&pop=%s",
                     CONFIG_PAIRBLE_SETUP_URL, s_device_name, s_pop);
    ESP_RETURN_ON_FALSE(n > 0 && (size_t)n < sizeof(s_setup_url), ESP_ERR_INVALID_SIZE, TAG,
                        "setup URL too long");

    ESP_ERROR_CHECK(esp_event_handler_register(NETWORK_PROV_EVENT, ESP_EVENT_ANY_ID, on_prov_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(PROTOCOMM_TRANSPORT_BLE_EVENT, ESP_EVENT_ANY_ID,
                                               on_ble_transport_event, NULL));
#if CONFIG_PAIRBLE_BLE_IDLE_TIMEOUT_SECONDS > 0
    const esp_timer_create_args_t idle_args = { .callback = idle_expired_cb, .name = "pairble_idle" };
    ESP_ERROR_CHECK(esp_timer_create(&idle_args, &s_idle_timer));
#endif

    /* A copy of the BLE scheme, because the manager switches Wi-Fi to the
     * scheme's mode whenever it applies credentials - STA for BLE, which would
     * take the SoftAP down at exactly the moment its client is waiting for the
     * answer. AP+STA keeps it up. */
    s_mgr_cfg = (network_prov_mgr_config_t){
        .scheme = network_prov_scheme_ble,
        .scheme_event_handler = { .event_cb = bt_memory_cb },
        .network_prov_wifi_conn_cfg = {
            .wifi_conn_attempts = CONFIG_PAIRBLE_WIFI_CONN_ATTEMPTS,
        },
    };
    if (s_config.softap) {
        s_mgr_cfg.scheme.wifi_mode = WIFI_MODE_APSTA;
    }

    const esp_timer_create_args_t timers[] = {
        { .callback = reboot_cb,         .name = "pairble_reboot" },
        { .callback = finish_cb,         .name = "pairble_finish" },
        { .callback = window_expired_cb, .name = "pairble_window" },
        { .callback = retry_cb,          .name = "pairble_retry" },
        { .callback = recovery_cb,       .name = "pairble_recovery" },
    };
    esp_timer_handle_t *handles[] = { &s_reboot_timer, &s_finish_timer, &s_window_timer, &s_retry_timer, &s_recovery_timer };
    for (size_t i = 0; i < sizeof(timers) / sizeof(timers[0]); i++) {
        ESP_ERROR_CHECK(esp_timer_create(&timers[i], handles[i]));
    }
    s_initialised = true;

    recover_interrupted_attempt();
    setup_mode_t boot_mode = SETUP_MANUAL;
    /* RTC_NOINIT holds whatever the last software reset left - or noise
     * after a power-on, which the magic value makes harmless but which is
     * not worth even that chance. */
    bool into_setup = esp_reset_reason() == ESP_RST_SW && (s_boot_into_setup & ~0xfu) == BOOT_INTO_SETUP;
    if (into_setup) {
        boot_mode = (setup_mode_t)(s_boot_into_setup & 0xfu);
    }
    s_boot_into_setup = 0;
    /* Only a device with a network to go back to has setup to reopen, and a
     * count left over from before it was reset must not carry into the
     * first power-ons after it is set up again. */
    if (!pairble_is_provisioned()) {
        pairble_nvs_erase("cycles");  /* no flash write when there is none */
    } else if (pairble_power_cycles_reached()) {
        into_setup = true;
        boot_mode = SETUP_MANUAL;
    }
    ESP_RETURN_ON_ERROR(pairble_button_start(), TAG, "setup button GPIO %d", CONFIG_PAIRBLE_SETUP_BUTTON_GPIO);
#if CONFIG_PAIRBLE_IMPROV_UART
    ESP_RETURN_ON_ERROR(pairble_improv_uart_start(), TAG, "Improv on UART %d", CONFIG_PAIRBLE_IMPROV_UART_NUM);
#endif

    if (pairble_is_provisioned() && !into_setup) {
        ESP_LOGI(TAG, "credentials stored, connecting");
        /* Setup never started, so release what CONFIG_PAIRBLE_BT_MEMORY says
         * as if it had started and stopped. */
        bt_memory_cb(NULL, NETWORK_PROV_INIT, NULL);
        bt_memory_cb(NULL, NETWORK_PROV_DEINIT, NULL);
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        prepare_station();
        /* Offline before a deep sleep: carry on counting from then (and a stop
     * during this wake-up must not forget it). */
    s_offline_since_before = s_offline_since_us;
        if (s_offline_since_us != 0) {
            arm_recovery(10);
        }
        return esp_wifi_start();
    }
    return open_setup(pairble_is_provisioned() ? boot_mode : SETUP_FIRST);
}

bool pairble_is_provisioned(void)
{
    if (s_recovery) {
        return true;  /* the manager has cleared the station config meanwhile */
    }
    bool provisioned = false;
    if (s_initialised) {
        network_prov_mgr_is_wifi_provisioned(&provisioned);
    }
    return provisioned;
}

bool pairble_is_setup_open(void)
{
    return s_provisioning;
}

bool pairble_is_online(void)
{
    return s_online;
}

const char *pairble_setup_url(void)
{
    return s_setup_url;
}

const char *pairble_wifi_qr(void)
{
    return s_wifi_qr[0] ? s_wifi_qr : NULL;
}

pairble_result_t pairble_current_result(void)
{
    portENTER_CRITICAL(&s_lock);
    pairble_result_t copy = s_result;
    portEXIT_CRITICAL(&s_lock);
    return copy;
}

void pairble_result_set_connecting(void)
{
    result_set(PAIRBLE_RESULT_CONNECTING, PAIRBLE_FAIL_NONE);
}

const char *pairble_device_name(void)
{
    return s_device_name;
}

static void open_setup_task(void *arg)
{
    esp_err_t err = open_setup((setup_mode_t)(uintptr_t)arg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not reopen setup: %s", esp_err_to_name(err));
    }
    s_opening = false;
    vTaskDelete(NULL);
}

static esp_err_t request_setup(setup_mode_t mode)
{
    ESP_RETURN_ON_FALSE(s_initialised, ESP_ERR_INVALID_STATE, TAG, "call pairble_start() first");
    if (s_closing) {
        if (s_provisioning) {
            return ESP_ERR_INVALID_STATE;  /* closing right now: ask again in a moment */
        }
        s_closing = false;  /* a stop that found nothing to stop */
    }
    portENTER_CRITICAL(&s_lock);
    bool busy = s_provisioning || s_opening;
    if (!busy) {
        s_opening = true;
    }
    portEXIT_CRITICAL(&s_lock);
    if (busy) {
        return ESP_OK;
    }
    if (!pairble_is_provisioned()) {
        mode = SETUP_FIRST;
    }
    if (s_bt_released) {
        ESP_LOGI(TAG, "restarting into setup (Bluetooth memory was released)");
        s_boot_into_setup = BOOT_INTO_SETUP | mode;
        esp_restart();
    }
    /* Starting the BLE stack takes a while and more stack than a timer
     * callback or the event loop has, and this is called from both. */
    if (xTaskCreate(open_setup_task, "pairble_setup", 6144, (void *)(uintptr_t)mode, 5, NULL) != pdPASS) {
        s_opening = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t pairble_open_setup(void)
{
    return request_setup(SETUP_MANUAL);
}

esp_err_t pairble_set_network(const char *ssid, const char *password, uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(ssid != NULL, ESP_ERR_INVALID_ARG, TAG, "no SSID");
    size_t ssid_len = strnlen(ssid, 33);
    size_t pass_len = password != NULL ? strnlen(password, 64) : 0;
    ESP_RETURN_ON_FALSE(ssid_len >= 1 && ssid_len <= 32, ESP_ERR_INVALID_ARG, TAG, "SSID must be 1-32 bytes");
    ESP_RETURN_ON_FALSE(pass_len <= 63, ESP_ERR_INVALID_ARG, TAG, "password must be at most 63 bytes");
    ESP_RETURN_ON_FALSE(s_provisioning && s_mgr_ready && !s_closing, ESP_ERR_INVALID_STATE, TAG, "setup is not open");

    ESP_RETURN_ON_FALSE(!s_creds_received && pairble_current_result().state != PAIRBLE_RESULT_CONNECTING,
                        ESP_ERR_INVALID_STATE, TAG, "an attempt is running");

    /* What a phone arriving does (phone_arrived()): no retry of the stored
     * network in the middle, and a station the new config can be applied to -
     * not one that is connected or connecting. */
    s_creds_received = true;
    esp_timer_stop(s_retry_timer);
    quiet_station();

    wifi_config_t cfg = { 0 };
    memcpy(cfg.sta.ssid, ssid, ssid_len);  /* exactly 32 bytes carries no NUL, and needs none */
    memcpy(cfg.sta.password, password, pass_len);
    s_from_app = true;
    pairble_result_set_connecting();  /* before: a quick failure must not be overwritten */
    /* The path credentials from a phone take, so everything after is the same. */
    esp_err_t err = network_prov_mgr_configure_wifi_sta(&cfg);
    memset(&cfg, 0, sizeof(cfg));
    if (err != ESP_OK) {
        s_from_app = false;
        s_creds_received = false;
        result_set(PAIRBLE_RESULT_IDLE, PAIRBLE_FAIL_NONE);
        if (s_recovery) {
            esp_timer_start_once(s_retry_timer, 1000 * 1000);  /* back to the stored network */
        }
        return ESP_ERR_INVALID_STATE;  /* an attempt has already succeeded */
    }
    if (timeout_ms == 0) {
        return ESP_OK;
    }

    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    for (;;) {
        pairble_result_t r = pairble_current_result();
        if (r.state == PAIRBLE_RESULT_CONNECTED) {
            return ESP_OK;
        }
        if (r.state == PAIRBLE_RESULT_FAILED) {
            return r.reason == PAIRBLE_FAIL_WRONG_PASSWORD ? ESP_ERR_WIFI_PASSWORD : ESP_ERR_WIFI_SSID;
        }
        if (esp_timer_get_time() >= deadline) {
            s_from_app = false;  /* a phone may finish this session; let it read the result */
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

esp_err_t pairble_stop(void)
{
    if (s_provisioning) {
        s_closing = true;
        network_prov_mgr_stop_provisioning();
    }
    return ESP_OK;
}

esp_err_t pairble_reset_credentials(void)
{
    ESP_RETURN_ON_FALSE(s_initialised, ESP_ERR_INVALID_STATE, TAG, "call pairble_start() first");
    pairble_nvs_erase("prev");
    pairble_nvs_erase("ap");
    pairble_nvs_erase("cycles");
    if (s_recovery) {
        /* Setup is open and would put the old network back when it closes. */
        memset(&s_stored, 0, sizeof(s_stored));
        s_recovery = false;
    }
    return esp_wifi_restore();
}
