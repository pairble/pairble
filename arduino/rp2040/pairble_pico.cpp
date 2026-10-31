/*
 * pairble on the Pico W: state, Wi-Fi, flash and the public API.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Everything here runs from pairble_loop(). The Bluetooth side only leaves
 * requests in pb_shared (want_scan, want_apply) for this to carry out:
 * scanning and joining go through the Arduino WiFi class, which must not be
 * called from the Bluetooth context. Joining is a state machine polled from
 * pairble_loop(), so nothing here holds the sketch up.
 */
#include <Arduino.h>
#include <WiFi.h>
#include <EEPROM.h>
#include <pico/cyw43_arch.h>
#include "pairble.h"
#include "pairble_pico_internal.h"

pb_shared_t pb_shared;

static const uint32_t JOIN_TIMEOUT_MS = 20000;
static const uint32_t RETRY_STORED_MS = 30000;
/* After a success setup closes as on the ESP32 (pairble.c): soon after the
 * phone that read the result hangs up, or after the SoftAP page had time to
 * get it; the other two for a phone that does neither. */
static const uint32_t CLOSE_AFTER_HANGUP_MS = 300;
static const uint32_t CLOSE_AFTER_HTTP_MS = 1500;
static const uint32_t CLOSE_AFTER_READ_MS = 15000;      /* read, never hung up */
static const uint32_t CLOSE_AFTER_SUCCESS_MS = 60000;   /* ... or never read   */
/* Closing waits for the phone's disconnect to complete before touching
 * Wi-Fi or the buffers a late request would use - for at most this long. */
static const uint32_t CLOSE_DISCONNECT_MS = 1500;
static const uint32_t SCAN_WAIT_MS = 10000;
static const uint32_t RESCAN_MS = 30000;

/* ---- flash ---------------------------------------------------------------- */

#define STORE_MAGIC 0x50424c31u  /* "PBL1" */

typedef struct {
    uint32_t magic;
    char pop[65];
    char ssid[33];
    char pass[65];
} store_t;

static store_t s_store;

static void store_load(void)
{
    EEPROM.begin(256);
    EEPROM.get(0, s_store);
    if (s_store.magic != STORE_MAGIC) {
        memset(&s_store, 0, sizeof(s_store));
        s_store.magic = STORE_MAGIC;
    }
    s_store.pop[sizeof(s_store.pop) - 1] = '\0';
    s_store.ssid[sizeof(s_store.ssid) - 1] = '\0';
    s_store.pass[sizeof(s_store.pass) - 1] = '\0';
}

static void store_save(void)
{
    EEPROM.put(0, s_store);
    EEPROM.commit();
}

/* ---- state ---------------------------------------------------------------- */

static pairble_config_t s_cfg;
static bool s_started;
static char s_name[PAIRBLE_NAME_MAX_LEN + 1];
static char s_url[192];
static char s_wifi_qr[128];
static uint8_t s_service_be[16];

static bool s_setup_open;
static uint32_t s_setup_opened_at;
static uint32_t s_success_at;        /* 0: no success to close setup for */
static uint32_t s_closing_at;        /* 0: not closing */
/* Opened on a provisioned board - by pairble_open_setup() or after time
 * offline: closes after recovery_window_seconds. */
static bool s_recovery;
static bool s_recovery_auto;         /* ... the latter: also once the stored network is back */
static bool s_reboot_pending;
static bool s_online;
static uint32_t s_offline_since;     /* 0: online, or never had a network */
static uint32_t s_last_join_try;
static bool s_scanning;
static uint32_t s_last_scan;

/* The one join in progress, if any. */
enum join_kind { JOIN_NONE, JOIN_STORED, JOIN_BOOT, JOIN_NEW };
static struct {
    join_kind kind;
    uint32_t start;
    int last_link;
    char ssid[33];
    char pass[65];
} s_join;

void pairble_lock(void) { async_context_acquire_lock_blocking(cyw43_arch_async_context()); }
void pairble_unlock(void) { async_context_release_lock(cyw43_arch_async_context()); }
uint32_t pairble_random32(void) { return rp2040.hwrand32(); }

static void emit(pairble_event_t event, pairble_fail_reason_t reason = PAIRBLE_FAIL_NONE)
{
    if (s_cfg.event_cb) s_cfg.event_cb(event, reason, s_cfg.user_ctx);
}

static bool parse_uuid(const char *text, uint8_t out[16])
{
    int n = 0;
    for (const char *p = text; *p; p++) {
        if (*p == '-') continue;
        int v = isdigit((unsigned char)*p) ? *p - '0'
              : isxdigit((unsigned char)*p) ? (tolower((unsigned char)*p) - 'a' + 10) : -1;
        if (v < 0 || n >= 32) return false;
        if (n % 2 == 0) out[n / 2] = v << 4;
        else out[n / 2] |= v;
        n++;
    }
    return n == 32;
}

static bool valid_chars(const char *s, const char *extra)
{
    for (; *s; s++) {
        if (!isalnum((unsigned char)*s) && !strchr(extra, *s)) return false;
    }
    return true;
}

/* Crockford base32 without I, L, O and U, twelve characters: as the ESP32
 * (pairble.c, generate_pop) and @pairble/cli. */
static void generate_pop(char *out)
{
    static const char alphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
    for (int i = 0; i < 12; i++) out[i] = alphabet[pairble_random32() & 31];
    out[12] = '\0';
}

/* ---- scanning ------------------------------------------------------------- */

void pb_request_scan(void)
{
    if (s_scanning || s_join.kind != JOIN_NONE) {
        pb_shared.want_scan = true;  /* after what is running now */
        return;
    }
    WiFi.scanNetworks(true);
    s_scanning = true;
    s_last_scan = millis();
}

bool pb_scanning(void) { return s_scanning || pb_shared.want_scan; }

uint32_t pb_since(uint32_t t)
{
    int32_t d = (int32_t)(millis() - t);
    return d > 0 ? (uint32_t)d : 0;
}

/* WiFiClass's scan auth values (lwIP_CYW43.cpp) to protocomm's WifiAuthMode. */
static uint8_t auth_mode(uint8_t enc)
{
    switch (enc) {
    case ENC_TYPE_NONE: return 0;  /* Open */
    case ENC_TYPE_TKIP: return 2;  /* WPA_PSK */
    case ENC_TYPE_CCMP: return 3;  /* WPA2_PSK */
    default:            return 4;  /* WPA_WPA2_PSK */
    }
}

static void finish_scan(void)
{
    int n = WiFi.scanComplete();
    if (n < 0) return;  /* still running */
    s_scanning = false;

    /* One entry per name, the strongest, strongest first: the phone shows
     * names, and the Pico joins by name. */
    pb_scan_entry_t *list = (pb_scan_entry_t *)calloc(PB_MAX_SCAN, sizeof(pb_scan_entry_t));
    if (!list) {
        WiFi.scanDelete();
        return;
    }
    int count = 0;
    for (int i = 0; i < n; i++) {
        const char *ssid = WiFi.SSID(i);
        if (!ssid || !*ssid || strlen(ssid) > 32) continue;
        int8_t rssi = WiFi.RSSI(i);
        int j = 0;
        while (j < count && strcmp(list[j].ssid, ssid) != 0) j++;
        if (j < count && list[j].rssi >= rssi) continue;
        if (j == count) {
            if (count == PB_MAX_SCAN) continue;
            count++;
        }
        pb_scan_entry_t *e = &list[j];
        strcpy(e->ssid, ssid);
        e->rssi = rssi;
        e->channel = WiFi.channel(i);
        WiFi.BSSID(i, e->bssid);
        e->auth = auth_mode(WiFi.encryptionType(i));
    }
    WiFi.scanDelete();
    for (int i = 1; i < count; i++) {
        pb_scan_entry_t e = list[i];
        int j = i;
        for (; j > 0 && list[j - 1].rssi < e.rssi; j--) list[j] = list[j - 1];
        list[j] = e;
    }

    pairble_lock();
    memcpy(pb_shared.scan, list, PB_MAX_SCAN * sizeof(pb_scan_entry_t));
    pb_shared.scan_count = count;
    pb_shared.scan_done = true;
    pairble_unlock();
    free(list);
}

static bool seen_in_scan(const char *ssid)
{
    for (int i = 0; i < pb_shared.scan_count; i++) {
        if (strcmp(pb_shared.scan[i].ssid, ssid) == 0) return true;
    }
    return false;
}

/* ---- joining -------------------------------------------------------------- */

static pairble_err_t open_setup(bool automatic);

static void set_online(bool online)
{
    if (online == s_online) return;
    s_online = online;
    if (online) {
        s_offline_since = 0;
        emit(PAIRBLE_EVENT_ONLINE);
    } else {
        s_offline_since = millis() | 1;
        emit(PAIRBLE_EVENT_OFFLINE, PAIRBLE_FAIL_CONNECTION_LOST);
    }
}

static void join_begin(join_kind kind, const char *ssid, const char *pass)
{
    if (s_scanning) {  /* the radio does one thing at a time */
        uint32_t start = millis();
        while (WiFi.scanComplete() < 0 && pb_since(start) < SCAN_WAIT_MS) delay(10);
        finish_scan();
        if (s_scanning) {  /* never finished: give up on it */
            pairble_lock();  /* the driver's callback adds to the same list */
            WiFi.scanDelete();
            pairble_unlock();
            s_scanning = false;
        }
    }
    s_join.kind = kind;
    strlcpy(s_join.ssid, ssid, sizeof(s_join.ssid));
    strlcpy(s_join.pass, pass, sizeof(s_join.pass));
    s_join.start = millis();
    s_join.last_link = 99;
    Serial.printf("pairble: joining %s\n", ssid);
    WiFi.disconnect();
    /* disconnect() leaves arduino-pico in AP-only mode while the access point
     * is up, and begin() in that mode starts an access point (an ESP8266
     * compatibility rule) - with the home network's name. */
    WiFi.mode(pb_softap_running() ? WIFI_AP_STA : WIFI_STA);
    WiFi.setTimeout(JOIN_TIMEOUT_MS);
    /* Does not wait once WiFi.mode() has been called; join_poll() does. */
    WiFi.begin(ssid, *pass ? pass : nullptr);
}

static void joined_new(void)
{
    uint8_t bssid[6];
    WiFi.BSSID(bssid);
    pairble_lock();
    strlcpy(pb_shared.ip, WiFi.localIP().toString().c_str(), sizeof(pb_shared.ip));
    strcpy(pb_shared.connected_ssid, s_join.ssid);
    memcpy(pb_shared.connected_bssid, bssid, 6);
    pb_shared.connected_channel = WiFi.channel();
    pb_shared.result_delivered = false;
    pb_shared.result_over_http = false;
    pb_shared.hangup_at = 0;
    pb_shared.sta_state = PB_STA_CONNECTED;
    pairble_unlock();

    strcpy(s_store.ssid, s_join.ssid);
    strcpy(s_store.pass, s_join.pass);
    memset(s_join.pass, 0, sizeof(s_join.pass));
    store_save();
    /* Setup closes after this (pairble_loop()); without setup open - Improv
     * on its own - there is nothing to close, and a reboot comes straight
     * after Improv's answer. */
    if (s_setup_open && !s_closing_at) s_success_at = millis() | 1;
    s_reboot_pending = s_cfg.reboot_after_success;

    Serial.printf("pairble: on %s, IP %s, at %lu ms\n", s_join.ssid, pb_shared.ip, (unsigned long)millis());
    emit(PAIRBLE_EVENT_SUCCESS);
    set_online(true);
}

static void failed_new(int link)
{
    memset(s_join.pass, 0, sizeof(s_join.pass));
    /* Which of the two the phone is told: the driver's last word, or, when
     * it gave none, whether the network was in the list at all. */
    bool not_found = link == CYW43_LINK_NONET ||
                     (link != CYW43_LINK_BADAUTH && pb_shared.scan_done && !seen_in_scan(s_join.ssid));
    Serial.printf("pairble: %s failed (link %d): %s\n", s_join.ssid, link, not_found ? "not found" : "wrong password");
    pairble_lock();
    pb_shared.sta_reason = not_found ? PB_REASON_NOT_FOUND : PB_REASON_AUTH;
    pb_shared.sta_state = PB_STA_FAILED;
    pairble_unlock();
    /* Not left half-joined (associated, no address yet), to get one later
     * while reported failed. disconnect() leaves WiFiClass in AP or OFF
     * mode; join_begin() and pb_softap_stop() set the mode they need. */
    WiFi.disconnect();
    emit(PAIRBLE_EVENT_FAILED, not_found ? PAIRBLE_FAIL_AP_NOT_FOUND : PAIRBLE_FAIL_WRONG_PASSWORD);
    s_last_join_try = 0;  /* back to the stored network, if any, right away */
}

static void join_poll(void)
{
    if (s_join.kind == JOIN_NONE) return;
    /* Joined: associated (JOIN) and an address from DHCP. Not
     * WiFi.status(), which says connected whenever WiFiClass is in AP mode,
     * and not link 0, which is CYW43_LINK_DOWN. */
    int link = cyw43_wifi_link_status(&cyw43_state, CYW43_ITF_STA);
    bool joined = link == CYW43_LINK_JOIN && WiFi.localIP().isSet();
    if (!joined) {
        if (link != s_join.last_link) {
            Serial.printf("pairble: link %d after %lu ms\n", link, (unsigned long)pb_since(s_join.start));
            s_join.last_link = link;
        }
        bool over = link == CYW43_LINK_BADAUTH || link == CYW43_LINK_NONET || link == CYW43_LINK_FAIL;
        if (!over && pb_since(s_join.start) < JOIN_TIMEOUT_MS) return;
        if (!over) link = CYW43_LINK_FAIL;  /* still joining, or down */
    }
    join_kind kind = s_join.kind;
    s_join.kind = JOIN_NONE;
    if (kind == JOIN_NEW) {
        if (joined) joined_new();
        else failed_new(link);
    } else {
        /* Cleared before any event: a callback may start the next join,
         * whose password must stay. */
        memset(s_join.pass, 0, sizeof(s_join.pass));
        if (!joined) {
            WiFi.disconnect();
            if (!s_offline_since) s_offline_since = millis() | 1;
            if (kind == JOIN_BOOT) {
                emit(PAIRBLE_EVENT_OFFLINE, PAIRBLE_FAIL_CONNECTION_LOST);
                open_setup(true);  /* as on the ESP32: no network at boot, setup */
            }
        }
        set_online(joined);
    }
}

/* A network from any of the ways in - a phone over Bluetooth or the access
 * point, Improv: try it, keep it if it works. */
void pb_request_network(const char *ssid, const char *pass)
{
    pairble_lock();
    pb_shared.sta_state = PB_STA_CONNECTING;
    pb_shared.attempts_remaining = 0;
    pairble_unlock();
    emit(PAIRBLE_EVENT_CREDENTIALS_RECEIVED);
    set_online(false);  /* the stored network is let go of for this one */
    join_begin(JOIN_NEW, ssid, pass);
}

/* ---- setup ---------------------------------------------------------------- */

static void finish_close(void);

static pairble_err_t open_setup(bool automatic)
{
    if (!s_started) return PAIRBLE_ERR_INVALID_STATE;
    if (s_closing_at) finish_close();  /* a close still under way: done now, then open again */
    if (s_setup_open) return PAIRBLE_OK;
    if (!pb_ble_start(s_name, s_service_be) || !pb_ble_buffers(true)) {
        Serial.println("pairble: no memory for setup");
        return PAIRBLE_ERR_NO_MEM;
    }
    s_recovery = s_store.ssid[0] != '\0';
    s_recovery_auto = s_recovery && automatic;
    s_reboot_pending = false;  /* a success from before is not this setup's */
    pairble_lock();
    pb_shared.sta_state = PB_STA_DISCONNECTED;
    pb_shared.new_ssid[0] = '\0';
    pb_shared.want_apply = false;
    pairble_unlock();
    pb_proto_new_keypair();
    pb_ble_advertise(true);
#if CONFIG_PAIRBLE_SOFTAP
    if (s_cfg.softap) pb_softap_start(s_name, s_store.pop);
#endif
    s_setup_open = true;
    s_setup_opened_at = millis();
    s_success_at = 0;
    pb_request_scan();
    Serial.printf("pairble: setup open as %s\n", s_name);
    emit(PAIRBLE_EVENT_STARTED);
    return PAIRBLE_OK;
}

pairble_err_t pairble_open_setup(void)
{
    return open_setup(false);
}

/* In two steps: the phone is let go of here, and the rest - the buffers its
 * last requests would use, the access point, which takes the radio off
 * Bluetooth - once it has gone (pairble_loop()). */
pairble_err_t pairble_stop(void)
{
    if (!s_setup_open || s_closing_at) return PAIRBLE_OK;
    pb_ble_advertise(false);
    pb_ble_disconnect();
    s_closing_at = millis() | 1;
    return PAIRBLE_OK;
}

static void finish_close(void)
{
    s_closing_at = 0;
    pb_shared.want_scan = false;
    pb_ble_buffers(false);
#if CONFIG_PAIRBLE_SOFTAP
    pb_softap_stop();
#endif
    s_setup_open = false;
    s_success_at = 0;
    Serial.printf("pairble: setup closed at %lu ms\n", (unsigned long)millis());
    emit(PAIRBLE_EVENT_STOPPED);
}

pairble_err_t pairble_reset_credentials(void)
{
    if (!s_started) return PAIRBLE_ERR_INVALID_STATE;
    s_store.ssid[0] = '\0';
    s_store.pass[0] = '\0';
    store_save();
    if (s_join.kind == JOIN_NEW) {  /* the phone is told it did not work */
        pairble_lock();
        pb_shared.sta_state = PB_STA_DISCONNECTED;
        pairble_unlock();
    }
    s_join.kind = JOIN_NONE;
    memset(s_join.pass, 0, sizeof(s_join.pass));
    WiFi.disconnect();
    set_online(false);
    s_offline_since = 0;
    s_recovery = s_recovery_auto = false;  /* open setup is now first-time setup */
    return PAIRBLE_OK;
}

pairble_err_t pairble_set_network(const char *ssid, const char *password, uint32_t timeout_ms)
{
    if (!ssid || !*ssid || strlen(ssid) > 32 || (password && strlen(password) > 63)) return PAIRBLE_ERR_INVALID_ARG;
    if (!pairble_is_setup_open() || s_join.kind == JOIN_NEW || pb_shared.want_apply ||
        pb_shared.sta_state == PB_STA_CONNECTING) {
        return PAIRBLE_ERR_INVALID_STATE;
    }
    pb_request_network(ssid, password ? password : "");
    if (timeout_ms == 0) return PAIRBLE_OK;
    uint32_t start = millis();
    while (pb_shared.sta_state == PB_STA_CONNECTING) {
        if (pb_since(start) > timeout_ms) return PAIRBLE_ERR_TIMEOUT;
        pairble_loop();
        delay(10);
    }
    if (pb_shared.sta_state == PB_STA_CONNECTED) {
        /* The caller has the result: setup closes as after the SoftAP page. */
        pairble_lock();
        pb_shared.result_delivered_at = millis();
        pb_shared.result_over_http = true;
        pb_shared.result_delivered = true;
        pairble_unlock();
        return PAIRBLE_OK;
    }
    if (pb_shared.sta_state != PB_STA_FAILED) return PAIRBLE_ERR_INVALID_STATE;  /* reset meanwhile */
    return pb_shared.sta_reason == PB_REASON_AUTH ? PAIRBLE_ERR_WRONG_PASSWORD : PAIRBLE_ERR_NOT_FOUND;
}

/* ---- API ------------------------------------------------------------------ */

pairble_err_t pairble_start(const pairble_config_t *config)
{
    if (s_started) return PAIRBLE_ERR_INVALID_STATE;
    const pairble_config_t defaults = PAIRBLE_CONFIG_DEFAULT();
    s_cfg = config ? *config : defaults;
    if (s_cfg.recovery_window_seconds == 0) {  /* a zeroed struct, as on the ESP32 */
        s_cfg.recovery_window_seconds = CONFIG_PAIRBLE_RECOVERY_WINDOW_SECONDS;
    }
#if !CONFIG_PAIRBLE_SOFTAP
    s_cfg.softap = false;
#endif

    store_load();
    if (s_cfg.pop) {
        strlcpy(s_store.pop, s_cfg.pop, sizeof(s_store.pop));
    } else if (!s_store.pop[0]) {
        generate_pop(s_store.pop);
        store_save();
        Serial.println("pairble: generated a proof of possession for this unit");
    }
    size_t pop_len = strlen(s_store.pop);
    if (pop_len < 8 || pop_len > 63 || !valid_chars(s_store.pop, "._~-")) {
        Serial.println("pairble: PoP must be 8-63 characters of A-Z, a-z, 0-9 and ._~-");
        return PAIRBLE_ERR_INVALID_ARG;
    }

    WiFi.mode(WIFI_STA);
    if (s_cfg.name) {
        strlcpy(s_name, s_cfg.name, sizeof(s_name));
    } else {
        const char *prefix = s_cfg.name_prefix ? s_cfg.name_prefix : CONFIG_PAIRBLE_NAME_PREFIX;
        if (strlen(prefix) > PAIRBLE_NAME_MAX_LEN - 6) {
            Serial.println("pairble: name prefix too long");
            return PAIRBLE_ERR_INVALID_ARG;
        }
        uint8_t mac[6];
        cyw43_arch_enable_sta_mode();  /* the MAC is read when the radio starts */
        cyw43_wifi_get_mac(&cyw43_state, CYW43_ITF_STA, mac);
        snprintf(s_name, sizeof(s_name), "%s%02X%02X%02X", prefix, mac[3], mac[4], mac[5]);
    }
    if (!*s_name || !valid_chars(s_name, "-_")) {
        Serial.println("pairble: name must be 1-29 characters of A-Z, a-z, 0-9, '-' and '_'");
        return PAIRBLE_ERR_INVALID_ARG;
    }
    if (!parse_uuid(CONFIG_PAIRBLE_SERVICE_UUID, s_service_be)) {
        Serial.println("pairble: CONFIG_PAIRBLE_SERVICE_UUID is not a valid 128-bit UUID");
        return PAIRBLE_ERR_INVALID_ARG;
    }
    /* Fragment, not query: the PoP stays out of the web server's logs. */
    snprintf(s_url, sizeof(s_url), "%s#name=%s&pop=%s", CONFIG_PAIRBLE_SETUP_URL, s_name, s_store.pop);
    /* Neither the name nor the PoP has a character the format escapes. */
    snprintf(s_wifi_qr, sizeof(s_wifi_qr), "WIFI:S:%s;T:WPA;P:%s;;", s_name, s_store.pop);
    pb_proto_init(s_store.pop);
    s_started = true;

    if (s_store.ssid[0]) {
        s_last_join_try = millis() | 1;
        join_begin(JOIN_BOOT, s_store.ssid, s_store.pass);  /* setup opens if this fails */
    } else {
        open_setup(false);
    }
    return PAIRBLE_OK;
}

void pairble_loop(void)
{
    if (!s_started) return;

    pb_ble_log();
    join_poll();
    if (s_scanning) finish_scan();
    /* Not while closing: a phone's late ScanStart (finish_close() drops
     * it). Improv asks with setup closed too. */
    if (pb_shared.want_scan && !s_closing_at && !s_scanning && s_join.kind == JOIN_NONE) {
        pb_shared.want_scan = false;
        pb_request_scan();
    }
    pb_improv_loop();

    if (s_closing_at) {
        /* Again: a phone may have connected while advertising was being
         * turned off. */
        pb_ble_disconnect();
        if (!pb_ble_connected() || pb_since(s_closing_at) > CLOSE_DISCONNECT_MS) finish_close();
    }

    if (s_setup_open && !s_closing_at) {
        pb_proto_new_keypair();  /* ready for the next phone */
#if CONFIG_PAIRBLE_SOFTAP
        pb_softap_loop();
#endif
        if (!s_scanning && s_join.kind == JOIN_NONE && pb_since(s_last_scan) > RESCAN_MS && !pb_ble_connected()) {
            pb_request_scan();
        }
        if (pb_shared.want_reprov) {
            pairble_lock();
            pb_shared.want_reprov = false;
            pb_shared.sta_state = PB_STA_DISCONNECTED;
            pairble_unlock();
        }
        if (pb_shared.want_apply) {
            char ssid[33], pass[65];
            pairble_lock();
            pb_shared.want_apply = false;
            strcpy(ssid, pb_shared.new_ssid);
            strcpy(pass, pb_shared.new_pass);
            pairble_unlock();
            pb_request_network(ssid, pass);
            memset(pass, 0, sizeof(pass));
        }
        if (s_success_at) {
            bool read = pb_shared.result_delivered;
            if ((pb_shared.hangup_at && pb_since(pb_shared.hangup_at) > CLOSE_AFTER_HANGUP_MS) ||
                (read && pb_shared.result_over_http && pb_since(pb_shared.result_delivered_at) > CLOSE_AFTER_HTTP_MS) ||
                (read && pb_since(pb_shared.result_delivered_at) > CLOSE_AFTER_READ_MS) ||
                pb_since(s_success_at) > CLOSE_AFTER_SUCCESS_MS) {
                pairble_stop();
            }
        } else {
            /* Not while a phone is at it, or a network is being tried. */
            uint32_t window = s_recovery ? s_cfg.recovery_window_seconds : s_cfg.window_seconds;
            if (window && !pb_ble_connected() && !pb_softap_has_clients() && s_join.kind != JOIN_NEW &&
                pb_since(s_setup_opened_at) > window * 1000UL) {
                /* Still offline: a full recovery_after_seconds before it
                 * opens again, not at once. */
                if (s_recovery && s_offline_since) s_offline_since = millis() | 1;
                pairble_stop();
            }
        }
    }

    /* reboot_after_success: once setup has closed, the phone (or Improv,
     * answered above) having its result. */
    if (s_reboot_pending && !s_setup_open) {
        /* A moment for what is still being sent - on any port Improv uses,
         * and by the sketch's own SUCCESS and ONLINE handlers. */
        uint32_t start = millis();
        while (pb_since(start) < 500) delay(10);
        Serial.flush();
        rp2040.reboot();
    }

    /* The stored network: watch it, and win it back when it goes. Not while
     * a phone is connected - it is about to set one anyway. */
    if (!s_store.ssid[0] || s_join.kind != JOIN_NONE) return;
    if (s_online) {
        set_online(WiFi.connected());
        /* Setup that opened by itself closes once the network is back, as
         * on the ESP32 - unless a phone is using it. */
        if (s_online && s_setup_open && !s_closing_at && s_recovery_auto && !s_success_at &&
            !pb_ble_connected() && !pb_softap_has_clients()) {
            pairble_stop();
        }
        return;
    }
    if (!s_setup_open && s_cfg.recovery_after_seconds && s_offline_since &&
        pb_since(s_offline_since) > s_cfg.recovery_after_seconds * 1000UL) {
        open_setup(true);
    }
    if (!(s_setup_open && (pb_ble_connected() || pb_softap_has_clients())) &&
        (s_last_join_try == 0 || pb_since(s_last_join_try) > RETRY_STORED_MS)) {
        s_last_join_try = millis() | 1;
        join_begin(JOIN_STORED, s_store.ssid, s_store.pass);
    }
}

const char *pairble_setup_url(void) { return s_url; }
const char *pairble_wifi_qr(void) { return s_cfg.softap ? s_wifi_qr : NULL; }
const char *pairble_device_name(void) { return s_name; }
bool pairble_is_provisioned(void) { return s_store.ssid[0] != '\0'; }
bool pairble_is_online(void) { return s_online; }
bool pairble_is_setup_open(void) { return s_setup_open && !s_closing_at; }
