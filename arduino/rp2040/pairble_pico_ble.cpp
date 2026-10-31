/*
 * The provisioning GATT service on BTstack, laid out as ESP-IDF's protocomm
 * NimBLE transport lays it out (components/protocomm/src/transports/
 * protocomm_nimble.c), so a client finds the same thing on either chip:
 *
 *   - one primary service with the configured 128-bit UUID, advertised;
 *   - one characteristic per endpoint, read and write, its UUID the service
 *     UUID with bytes 2-3 (as written) replaced by 0xFF4F + index;
 *   - a User Description (0x2901) on each, holding the endpoint name, which
 *     is how clients find them;
 *   - write the request, then read the response from the same characteristic.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <Arduino.h>
#include <btstack.h>
#include <ble/att_db_util.h>
#include <pico/cyw43_arch.h>
#include "pairble.h"
#include "pairble_pico_internal.h"

static uint16_t s_value_handle[PB_ENDPOINTS];
/* Allocated while setup is open (pb_ble_buffers()), so the RAM is back once
 * the board is on its network. */
typedef struct {
    uint8_t response[PB_ENDPOINTS][PB_MAX_PAYLOAD];
    uint8_t prepared[PB_MAX_PAYLOAD];        /* a long write, assembled */
} buffers_t;
static buffers_t *s_buf;
static uint16_t s_response_len[PB_ENDPOINTS];
static uint16_t s_prepared_len;
static int s_prepared_ep = -1;       /* where the long write in s_buf->prepared goes */
static bool s_prepared_bad;
static hci_con_handle_t s_conn = HCI_CON_HANDLE_INVALID;
static btstack_packet_callback_registration_t s_hci_cb;
static bool s_started;

// Bluetooth calls from loop() hold the CYW43 async context off.
struct PbLock {
    PbLock() { pairble_lock(); }
    ~PbLock() { pairble_unlock(); }
};

/* What happened on the link, for pb_ble_log() to print from loop(): Serial
 * is not for the Bluetooth context. */
static volatile uint8_t s_log_connects, s_log_disconnects, s_log_reason;
static volatile uint16_t s_log_mtu;
static volatile uint16_t s_log_interval, s_log_timeout;  /* 1.25 ms, 10 ms */
static volatile uint8_t s_log_params;
static volatile uint16_t s_log_requests;
static volatile uint8_t s_log_failures, s_log_failed_ep;

static bool s_disconnect_asked;      /* for s_conn */
static uint16_t s_changed_cccd;      /* Service Changed's CCCD: indications never sent */

static int endpoint_of(uint16_t handle)
{
    for (int i = 0; i < PB_ENDPOINTS; i++) {
        if (s_value_handle[i] == handle) return i;
    }
    return -1;
}

static bool s_response_has_result[PB_ENDPOINTS];  /* says CONNECTED */

static uint16_t on_read(hci_con_handle_t, uint16_t handle, uint16_t offset, uint8_t *buffer, uint16_t size)
{
    if (handle && handle == s_changed_cccd) {
        static const uint8_t off[2] = { 0, 0 };
        return att_read_callback_handle_blob(off, sizeof(off), offset, buffer, size);
    }
    int ep = endpoint_of(handle);
    if (ep < 0 || !s_buf) return 0;
    uint16_t n = att_read_callback_handle_blob(s_buf->response[ep], s_response_len[ep], offset, buffer, size);
    /* The last part of an answer saying CONNECTED is going out: setup can
     * close soon (buffer is NULL when BTstack only asks for the length). */
    if (buffer && s_response_has_result[ep] && offset + n >= s_response_len[ep]) {
        s_response_has_result[ep] = false;
        if (!pb_shared.result_delivered) {
            pb_shared.result_delivered_at = millis();
            pb_shared.result_delivered = true;
        }
    }
    return n;
}

static int handle_request(int ep, const uint8_t *data, uint16_t len)
{
    size_t out_len = 0;
    s_log_requests++;
    pb_shared.result_in_response = false;
    s_response_has_result[ep] = false;
    if (pb_proto_handle(ep, data, len, s_buf->response[ep], PB_MAX_PAYLOAD, &out_len) != 0) {
        s_log_failed_ep = ep;
        s_log_failures++;
        s_response_len[ep] = 0;
        return ATT_ERROR_UNLIKELY_ERROR;
    }
    s_response_len[ep] = (uint16_t)out_len;
    s_response_has_result[ep] = pb_shared.result_in_response;
    return 0;
}

static int on_write(hci_con_handle_t, uint16_t handle, uint16_t mode, uint16_t offset, uint8_t *buffer, uint16_t size)
{
    /* The end of a long write comes with handle 0 (att_server.c), so these
     * go by what the parts were for. The request is handled at VALIDATE,
     * the one step whose error BTstack passes back to the phone. */
    switch (mode) {
    case ATT_TRANSACTION_MODE_VALIDATE: {
        int ep = s_prepared_ep;
        uint16_t len = s_prepared_len;
        bool bad = s_prepared_bad;
        s_prepared_ep = -1;
        s_prepared_len = 0;
        if (ep < 0 || len == 0) return 0;
        if (bad) return ATT_ERROR_INVALID_ATTRIBUTE_VALUE_LENGTH;
        if (!s_buf) return ATT_ERROR_UNLIKELY_ERROR;
        return handle_request(ep, s_buf->prepared, len);
    }
    case ATT_TRANSACTION_MODE_EXECUTE:
        return 0;
    case ATT_TRANSACTION_MODE_CANCEL:
        s_prepared_ep = -1;
        s_prepared_len = 0;
        return 0;
    default:
        break;
    }

    int ep = endpoint_of(handle);
    if (ep < 0) return 0;
    if (!s_buf) return ATT_ERROR_UNLIKELY_ERROR;  /* setup closed */

    if (mode == ATT_TRANSACTION_MODE_ACTIVE) {  /* one part of a long write */
        if (offset == 0) { s_prepared_len = 0; s_prepared_bad = false; s_prepared_ep = ep; }
        if (ep != s_prepared_ep || offset != s_prepared_len || (size_t)offset + size > sizeof(s_buf->prepared)) {
            s_prepared_bad = true;
            return ATT_ERROR_INVALID_OFFSET;
        }
        memcpy(s_buf->prepared + offset, buffer, size);
        s_prepared_len = offset + size;
        return 0;
    }
    if (size == 0) return 0;  /* as protocomm_nimble.c */
    return handle_request(ep, buffer, size);
}

void pb_ble_log(void)
{
    static uint8_t connects, disconnects;
    static uint16_t mtu;
    if (s_log_connects != connects) {
        connects = s_log_connects;
        Serial.println("pairble: phone connected over Bluetooth");
    }
    static uint8_t params;
    if (s_log_params != params) {
        params = s_log_params;
        Serial.printf("pairble: link interval %u.%02u ms, timeout %u ms\n", s_log_interval * 125 / 100,
                      s_log_interval * 125 % 100, s_log_timeout * 10);
    }
    if (s_log_mtu != mtu) {
        mtu = s_log_mtu;
        Serial.printf("pairble: ATT MTU %u\n", mtu);
    }
    static uint8_t failures;
    if (s_log_failures != failures) {
        failures = s_log_failures;
        Serial.printf("pairble: request on %s refused\n", pb_endpoint_names[s_log_failed_ep]);
    }
    static bool delivered;
    if (pb_shared.result_delivered != delivered) {
        delivered = pb_shared.result_delivered;
        if (delivered) {
            Serial.printf("pairble: result read %s at %lu ms\n", pb_shared.result_over_http ? "over HTTP" : "over Bluetooth",
                          (unsigned long)pb_shared.result_delivered_at);
        }
    }
    if (s_log_disconnects != disconnects) {
        disconnects = s_log_disconnects;
        Serial.printf("pairble: phone disconnected (reason 0x%02x) after %u requests, at %lu ms\n", s_log_reason,
                      s_log_requests, (unsigned long)millis());
        s_log_requests = 0;
    }
}

/* The ATT server passes on connection events too: only the MTU from here. */
static void on_att(uint8_t type, uint16_t, uint8_t *packet, uint16_t)
{
    if (type == HCI_EVENT_PACKET && hci_event_packet_get_type(packet) == ATT_EVENT_MTU_EXCHANGE_COMPLETE) {
        s_log_mtu = att_event_mtu_exchange_complete_get_MTU(packet);
    }
}

static void on_hci(uint8_t type, uint16_t, uint8_t *packet, uint16_t)
{
    if (type != HCI_EVENT_PACKET) return;
    switch (hci_event_packet_get_type(packet)) {
    case HCI_EVENT_LE_META:
        if (hci_event_le_meta_get_subevent_code(packet) == HCI_SUBEVENT_LE_CONNECTION_COMPLETE) {
            if (hci_subevent_le_connection_complete_get_status(packet) != 0) break;
            s_conn = hci_subevent_le_connection_complete_get_connection_handle(packet);
            s_disconnect_asked = false;
            pb_proto_reset_session();
            s_log_connects++;
            s_log_interval = hci_subevent_le_connection_complete_get_conn_interval(packet);
            s_log_timeout = hci_subevent_le_connection_complete_get_supervision_timeout(packet);
            s_log_params++;
            /* A longer supervision timeout than a Mac gives (2 s): joining a
             * network takes the shared radio off Bluetooth for long enough
             * that the phone otherwise drops the link mid-join. 15-30 ms and
             * 6 s, within Apple's limits for accessories. */
            gap_request_connection_parameter_update(s_conn, 12, 24, 0, 600);
        } else if (hci_event_le_meta_get_subevent_code(packet) == HCI_SUBEVENT_LE_CONNECTION_UPDATE_COMPLETE) {
            s_log_interval = hci_subevent_le_connection_update_complete_get_conn_interval(packet);
            s_log_timeout = hci_subevent_le_connection_update_complete_get_supervision_timeout(packet);
            s_log_params++;
        }
        break;
    case HCI_EVENT_DISCONNECTION_COMPLETE:
        if (hci_event_disconnection_complete_get_connection_handle(packet) != s_conn) break;
        s_log_reason = hci_event_disconnection_complete_get_reason(packet);
        s_log_disconnects++;
        /* The phone that read the result has gone: setup can close. Not a
         * phone that dropped before reading it - it may be back. */
        if (pb_shared.result_delivered && !pb_shared.result_over_http && !pb_shared.hangup_at) {
            pb_shared.hangup_at = millis() | 1;
        }
        s_conn = HCI_CON_HANDLE_INVALID;
        s_disconnect_asked = false;
        pb_proto_reset_session();
        for (int i = 0; i < PB_ENDPOINTS; i++) {
            s_response_len[i] = 0;
            s_response_has_result[i] = false;
        }
        s_prepared_ep = -1;
        s_prepared_len = 0;
        break;
    default:
        break;
    }
}

bool pb_ble_start(const char *name, const uint8_t service_be[16])
{
    if (s_started) return true;
    PbLock lock;

    att_db_util_init();
    att_db_util_add_service_uuid16(ORG_BLUETOOTH_SERVICE_GENERIC_ACCESS);
    att_db_util_add_characteristic_uuid16(ORG_BLUETOOTH_CHARACTERISTIC_GAP_DEVICE_NAME, ATT_PROPERTY_READ,
                                          ATT_SECURITY_NONE, ATT_SECURITY_NONE, (uint8_t *)name, strlen(name));
    att_db_util_add_service_uuid128(service_be);
    for (int i = 0; i < PB_ENDPOINTS; i++) {
        uint8_t uuid[16];
        memcpy(uuid, service_be, 16);
        uint16_t u16 = 0xFF4F + i;
        uuid[2] = u16 >> 8;
        uuid[3] = u16 & 0xff;
        s_value_handle[i] = att_db_util_add_characteristic_uuid128(
            uuid, ATT_PROPERTY_READ | ATT_PROPERTY_WRITE | ATT_PROPERTY_DYNAMIC,
            ATT_SECURITY_NONE, ATT_SECURITY_NONE, NULL, 0);
        att_db_util_add_descriptor_uuid16(ORG_BLUETOOTH_DESCRIPTOR_GATT_CHARACTERISTIC_USER_DESCRIPTION,
                                          ATT_PROPERTY_READ, ATT_SECURITY_NONE, ATT_SECURITY_NONE,
                                          (uint8_t *)pb_endpoint_names[i], strlen(pb_endpoint_names[i]));
    }
    /* Service Changed, as NimBLE has it (ble_svc_gatt_init()): with an
     * address that stays the same, phones would otherwise keep the handles
     * of an older firmware. Last, so the handles before it keep theirs. */
    att_db_util_add_service_uuid16(ORG_BLUETOOTH_SERVICE_GENERIC_ATTRIBUTE);
    uint16_t size_before = att_db_util_get_size();
    static uint8_t changed_range[4];
    uint16_t changed = att_db_util_add_characteristic_uuid16(ORG_BLUETOOTH_CHARACTERISTIC_GATT_SERVICE_CHANGED,
                                                             ATT_PROPERTY_INDICATE, ATT_SECURITY_NONE,
                                                             ATT_SECURITY_NONE, changed_range, sizeof(changed_range));
    s_changed_cccd = changed ? changed + 1 : 0;  /* att_db_util adds it right after */
    if (s_value_handle[PB_ENDPOINTS - 1] == 0 || changed == 0 ||
        /* att_db_util skips what does not fit without a word: declaration
         * 13 + value 12 + CCCD 10 bytes, or something was left out. */
        att_db_util_get_size() - size_before != 13 + 12 + 10) {
        Serial.println("pairble: GATT database too large (MAX_ATT_DB_SIZE)");
        return false;
    }

    l2cap_init();
    sm_init();
    att_server_init(att_db_util_get_address(), on_read, on_write);
    s_hci_cb.callback = on_hci;
    hci_add_event_handler(&s_hci_cb);
    att_server_register_packet_handler(on_att);

    /* Advertising: flags and the service UUID; the name in the scan response,
     * where there is room for all 29 characters. */
    static uint8_t adv[3 + 18];
    static uint8_t rsp[2 + PAIRBLE_NAME_MAX_LEN];
    adv[0] = 2; adv[1] = BLUETOOTH_DATA_TYPE_FLAGS; adv[2] = 0x06;
    adv[3] = 17; adv[4] = BLUETOOTH_DATA_TYPE_COMPLETE_LIST_OF_128_BIT_SERVICE_CLASS_UUIDS;
    for (int i = 0; i < 16; i++) adv[5 + i] = service_be[15 - i];  /* little-endian on air */
    size_t n = strlen(name);
    rsp[0] = n + 1; rsp[1] = BLUETOOTH_DATA_TYPE_COMPLETE_LOCAL_NAME;
    memcpy(rsp + 2, name, n);
    /* An LE-only device to whoever scans. The CYW43 is dual-mode and the
     * core builds BTstack with Classic, and a Mac that sees the same address
     * over BR/EDR takes the board for a Classic device: Chrome then finds it
     * only by a slow inquiry and refuses to connect ("Unsupported device").
     * So no Classic discovery or connections, and a static random address
     * for LE, which cannot be mistaken for the Classic one. */
#ifdef ENABLE_CLASSIC
    gap_discoverable_control(0);
    gap_connectable_control(0);
#endif
    /* Fixed per board, from its MAC, so phones and computers that have seen
     * it keep recognising it; set outright, because one BTstack generates
     * itself arrives after advertising has started without it. */
    uint8_t mac[6];
    cyw43_wifi_get_mac(&cyw43_state, CYW43_ITF_STA, mac);
    bd_addr_t random_addr;
    for (int i = 0; i < 6; i++) random_addr[i] = mac[5 - i] ^ 0x5a;
    gap_random_address_set(random_addr);  /* sets the top bits: static random */
    bd_addr_t none = {0};
    /* 30 ms, not protocomm_nimble.c's 160: the CYW43 shares its radio with
     * Wi-Fi and a phone hears only part of what it sends, so a slower rate
     * leaves it minutes from appearing in a browser's device list. Only for
     * as long as setup is open. */
    gap_advertisements_set_params(0x30, 0x30, 0, 0, none, 0x07, 0x00);
    gap_advertisements_set_data(sizeof(adv), adv);
    gap_scan_response_set_data(n + 2, rsp);

    hci_power_control(HCI_POWER_ON);
    s_started = true;
    return true;
}

bool pb_ble_buffers(bool on)
{
    buffers_t *buf = on ? (buffers_t *)calloc(1, sizeof(buffers_t)) : NULL;
    if (on && !buf) return false;
    PbLock lock;
    if (on && s_buf) {
        free(buf);
        return true;
    }
    free(s_buf);
    s_buf = buf;
    for (int i = 0; i < PB_ENDPOINTS; i++) {
        s_response_len[i] = 0;
        s_response_has_result[i] = false;
    }
    s_prepared_ep = -1;
    s_prepared_len = 0;
    return true;
}

void pb_ble_advertise(bool on)
{
    PbLock lock;
    gap_advertisements_enable(on ? 1 : 0);
}

/* Once per connection: called again while closing, for a phone that
 * connected meanwhile, and BTstack would send the command again each time. */
void pb_ble_disconnect(void)
{
    PbLock lock;
    if (s_conn != HCI_CON_HANDLE_INVALID && !s_disconnect_asked) {
        s_disconnect_asked = true;
        gap_disconnect(s_conn);
    }
}

bool pb_ble_connected(void)
{
    return s_conn != HCI_CON_HANDLE_INVALID;
}

