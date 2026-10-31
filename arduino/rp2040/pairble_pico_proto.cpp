/*
 * protocomm as ESP-IDF speaks it, for the Pico: security 1 and the
 * provisioning endpoints of network_provisioning, byte for byte.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The references are ESP-IDF's components/protocomm/src/security/security1.c
 * and network_provisioning's network_scan.c, network_config.c,
 * network_ctrl.c and manager.c; the schemas are vendored in
 * packages/core/proto. The client side is packages/core - whatever it
 * accepts from an ESP32, it must accept from here.
 */
#include <Arduino.h>
#include <string.h>
#include <bearssl/bearssl.h>
#include "pairble_pico_internal.h"

const char *const pb_endpoint_names[PB_ENDPOINTS] = {
    "prov-ctrl", "prov-scan", "prov-session", "prov-config", "proto-ver",
};
enum { EP_CTRL, EP_SCAN, EP_SESSION, EP_CONFIG, EP_VERSION };

/* manager.c, network_prov_get_info_json(), for security 1 with a PoP. */
static const char VERSION_JSON[] =
    "{\"prov\":{\"ver\":\"netprov-v1.2\",\"sec_ver\":1,\"sec_patch_ver\":0,\"cap\":[\"wifi_prov\",\"wifi_scan\"]}}";

/* Status (constants.proto) */
enum { ST_SUCCESS = 0, ST_INVALID_PROTO = 2, ST_INVALID_ARGUMENT = 4, ST_INTERNAL = 5 };

/* ---- protobuf, the little of it this needs ------------------------------ */

typedef struct {
    uint8_t *buf;
    size_t cap, len;
    bool overflow;
} pbw_t;

static void w_byte(pbw_t *w, uint8_t b)
{
    if (w->len < w->cap) w->buf[w->len++] = b;
    else w->overflow = true;
}

static void w_varint(pbw_t *w, uint64_t v)
{
    do {
        uint8_t b = v & 0x7f;
        v >>= 7;
        w_byte(w, v ? (b | 0x80) : b);
    } while (v);
}

static void w_tag(pbw_t *w, uint32_t field, uint8_t type) { w_varint(w, ((uint64_t)field << 3) | type); }

/* proto3 leaves a scalar at its default off the wire. */
static void w_uint(pbw_t *w, uint32_t field, uint64_t v)
{
    if (v) { w_tag(w, field, 0); w_varint(w, v); }
}

static void w_int32(pbw_t *w, uint32_t field, int32_t v)
{
    if (v) { w_tag(w, field, 0); w_varint(w, (uint64_t)(int64_t)v); }  /* negative: ten bytes */
}

/* Bytes, strings and sub-messages. A sub-message is a oneof member here, and
 * those go on the wire even when empty - that is how the client sees which
 * one arrived - so this always writes. */
static void w_bytes(pbw_t *w, uint32_t field, const void *data, size_t len)
{
    w_tag(w, field, 2);
    w_varint(w, len);
    for (size_t i = 0; i < len; i++) w_byte(w, ((const uint8_t *)data)[i]);
}

static void w_str(pbw_t *w, uint32_t field, const char *s)
{
    if (*s) w_bytes(w, field, s, strlen(s));
}

typedef struct {
    uint32_t no;
    uint8_t type;
    uint64_t v;
    const uint8_t *p;
    size_t len;
} pbf_t;

/* Next field of [*p, end). False at the end; *bad set on malformed input. */
static bool r_next(const uint8_t **p, const uint8_t *end, pbf_t *f, bool *bad)
{
    if (*p >= end) return false;
    uint64_t key = 0, v = 0;
    for (int shift = 0;; shift += 7) {
        if (*p >= end || shift > 63) { *bad = true; return false; }
        uint8_t b = *(*p)++;
        key |= (uint64_t)(b & 0x7f) << shift;
        if (!(b & 0x80)) break;
    }
    f->no = (uint32_t)(key >> 3);
    f->type = key & 7;
    if (f->type == 0 || f->type == 2) {
        for (int shift = 0;; shift += 7) {
            if (*p >= end || shift > 63) { *bad = true; return false; }
            uint8_t b = *(*p)++;
            v |= (uint64_t)(b & 0x7f) << shift;
            if (!(b & 0x80)) break;
        }
        f->v = v;
        if (f->type == 2) {
            if (v > (uint64_t)(end - *p)) { *bad = true; return false; }
            f->p = *p;
            f->len = (size_t)v;
            *p += v;
        }
    } else if (f->type == 1 || f->type == 5) {
        size_t n = f->type == 1 ? 8 : 4;
        if ((size_t)(end - *p) < n) { *bad = true; return false; }
        f->p = *p;
        f->len = n;
        *p += n;
    } else {
        *bad = true;
        return false;
    }
    return true;
}

#define FOR_FIELDS(f, data, size, bad) \
    for (const uint8_t *_p = (data), *_e = (data) + (size); r_next(&_p, _e, &(f), &(bad));)

/* ---- security 1 ---------------------------------------------------------- */

#define KEY_LEN 32

static char s_pop[65];
/* The session's key pair, and the next one, made ahead in loop(): X25519 on
 * an M0+ takes long enough to be felt inside a Bluetooth callback. */
static uint8_t s_dev_priv[KEY_LEN], s_dev_pub[KEY_LEN];
static uint8_t s_next_priv[KEY_LEN], s_next_pub[KEY_LEN];
static volatile bool s_keypair_fresh;
static uint8_t s_client_pub[KEY_LEN];
static enum { SESS_NONE, SESS_CMD0, SESS_READY } s_session;

/* AES-256-CTR with mbedtls's semantics: the whole 16-byte device random is
 * the initial counter block, incremented big-endian, and the keystream runs
 * on across messages (security1.c keeps nc_off). */
static br_aes_ct_cbcenc_keys s_aes;
static uint8_t s_ctr[16], s_stream[16];
static uint8_t s_stream_off;

static void ctr_xor(uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (s_stream_off == 16) {
            uint8_t iv[16] = {0};
            memcpy(s_stream, s_ctr, 16);
            br_aes_ct_cbcenc_run(&s_aes, iv, s_stream, 16);  /* one block of ECB */
            for (int j = 15; j >= 0 && ++s_ctr[j] == 0; j--) {}
            s_stream_off = 0;
        }
        data[i] ^= s_stream[s_stream_off++];
    }
}

static const br_ec_impl *ec(void)
{
#if defined(PICO_RP2350)
    return &br_ec_c25519_m31;    /* Cortex-M33: 32x32->64 multiply */
#else
    return &br_ec_c25519_m15;    /* Cortex-M0+ */
#endif
}

static void fill_random(uint8_t *out, size_t len)
{
    for (size_t i = 0; i < len; i += 4) {
        uint32_t r = pairble_random32();
        size_t n = len - i < 4 ? len - i : 4;
        memcpy(out + i, &r, n);
    }
}

void pb_proto_new_keypair(void)
{
    if (s_keypair_fresh) return;
    uint8_t priv[KEY_LEN], pub[KEY_LEN];
    fill_random(priv, KEY_LEN);
    ec()->mulgen(pub, priv, KEY_LEN, BR_EC_curve25519);
    pairble_lock();
    memcpy(s_next_priv, priv, KEY_LEN);
    memcpy(s_next_pub, pub, KEY_LEN);
    s_keypair_fresh = true;
    pairble_unlock();
}

void pb_proto_init(const char *pop)
{
    strncpy(s_pop, pop, sizeof(s_pop) - 1);
    s_session = SESS_NONE;
}

void pb_proto_reset_session(void)
{
    s_session = SESS_NONE;
}

static int session_cmd0(const uint8_t *client_pub, size_t len, pbw_t *w)
{
    if (len != KEY_LEN) return -1;
    if (s_keypair_fresh) {
        memcpy(s_dev_priv, s_next_priv, KEY_LEN);
        memcpy(s_dev_pub, s_next_pub, KEY_LEN);
        s_keypair_fresh = false;
    } else {  /* slow here, but never reuse one */
        fill_random(s_dev_priv, KEY_LEN);
        ec()->mulgen(s_dev_pub, s_dev_priv, KEY_LEN, BR_EC_curve25519);
    }
    memcpy(s_client_pub, client_pub, KEY_LEN);

    uint8_t key[KEY_LEN];
    memcpy(key, client_pub, KEY_LEN);
    if (!ec()->mul(key, KEY_LEN, s_dev_priv, KEY_LEN, BR_EC_curve25519)) return -1;
    if (s_pop[0]) {
        uint8_t digest[32];
        br_sha256_context sha;
        br_sha256_init(&sha);
        br_sha256_update(&sha, s_pop, strlen(s_pop));
        br_sha256_out(&sha, digest);
        for (int i = 0; i < KEY_LEN; i++) key[i] ^= digest[i];
    }
    uint8_t random[16];
    fill_random(random, sizeof(random));
    br_aes_ct_cbcenc_init(&s_aes, key, KEY_LEN);
    memcpy(s_ctr, random, 16);
    s_stream_off = 16;
    memset(key, 0, sizeof(key));
    s_session = SESS_CMD0;

    /* SessionData{ sec_ver=1, sec1=Sec1Payload{ msg=Session_Response0, sr0 } } */
    uint8_t sr0[80], sec1[96];
    pbw_t a = { sr0, sizeof(sr0), 0, false };
    w_bytes(&a, 2, s_dev_pub, KEY_LEN);
    w_bytes(&a, 3, random, 16);
    pbw_t b = { sec1, sizeof(sec1), 0, false };
    w_uint(&b, 1, 1);
    w_bytes(&b, 21, sr0, a.len);
    w_uint(w, 2, 1);
    w_bytes(w, 11, sec1, b.len);
    return 0;
}

static int session_cmd1(const uint8_t *client_verify, size_t len, pbw_t *w)
{
    if (s_session != SESS_CMD0 || len != KEY_LEN) return -1;
    uint8_t check[KEY_LEN];
    memcpy(check, client_verify, KEY_LEN);
    ctr_xor(check, KEY_LEN);
    /* A wrong proof of possession: fail the write, as security1.c does, and
     * the client reports it as such (packages/core/src/session.ts). */
    if (memcmp(check, s_dev_pub, KEY_LEN) != 0) {
        s_session = SESS_NONE;
        return -1;
    }
    uint8_t verify[KEY_LEN];
    memcpy(verify, s_client_pub, KEY_LEN);
    ctr_xor(verify, KEY_LEN);
    s_session = SESS_READY;

    uint8_t sr1[48], sec1[64];
    pbw_t a = { sr1, sizeof(sr1), 0, false };
    w_bytes(&a, 3, verify, KEY_LEN);
    pbw_t b = { sec1, sizeof(sec1), 0, false };
    w_uint(&b, 1, 3);
    w_bytes(&b, 23, sr1, a.len);
    w_uint(w, 2, 1);
    w_bytes(w, 11, sec1, b.len);
    return 0;
}

static int handle_session(const uint8_t *in, size_t len, pbw_t *w)
{
    pbf_t f, g, h;
    bool bad = false;
    uint64_t sec_ver = 0, msg = 0;
    const uint8_t *data = NULL;
    size_t data_len = 0;
    FOR_FIELDS(f, in, len, bad) {
        if (f.no == 2 && f.type == 0) sec_ver = f.v;
        if (f.no != 11 || f.type != 2) continue;
        FOR_FIELDS(g, f.p, f.len, bad) {
            if (g.no == 1 && g.type == 0) msg = g.v;
            if ((g.no == 20 || g.no == 22) && g.type == 2) {
                FOR_FIELDS(h, g.p, g.len, bad) {
                    /* sc0.client_pubkey = 1, sc1.client_verify_data = 2 */
                    if (h.type == 2 && h.no == (g.no == 20 ? 1u : 2u)) { data = h.p; data_len = h.len; }
                }
            }
        }
    }
    if (bad || sec_ver != 1 || data == NULL) return -1;
    if (msg == 0) return session_cmd0(data, data_len, w);
    if (msg == 2) return session_cmd1(data, data_len, w);
    return -1;
}

/* ---- prov-scan (network_scan.c) ----------------------------------------- */

static int handle_scan(const uint8_t *in, size_t len, pbw_t *w)
{
    pbf_t f, g;
    bool bad = false;
    uint64_t msg = 0, start = 0, count = 0;
    FOR_FIELDS(f, in, len, bad) {
        if (f.no == 1 && f.type == 0) msg = f.v;
        if (f.no == 14 && f.type == 2) {
            FOR_FIELDS(g, f.p, f.len, bad) {
                if (g.no == 1 && g.type == 0) start = g.v;
                if (g.no == 2 && g.type == 0) count = g.v;
            }
        }
    }
    if (bad) return -1;

    switch (msg) {
    case 0:   /* TypeCmdScanStart. The Pico keeps a list fresh while setup is
                 open, so this only asks for another; the client polls the
                 status either way. */
        pb_shared.want_scan = true;
        w_uint(w, 1, 1);
        w_bytes(w, 11, NULL, 0);
        return 0;
    case 2: { /* TypeCmdScanStatus */
        uint8_t body[16];
        pbw_t a = { body, sizeof(body), 0, false };
        w_uint(&a, 1, pb_shared.scan_done);
        w_uint(&a, 2, pb_shared.scan_done ? pb_shared.scan_count : 0);
        w_uint(w, 1, 3);
        w_bytes(w, 13, body, a.len);
        return 0;
    }
    case 4: { /* TypeCmdScanResult */
        uint8_t list[PB_MAX_PAYLOAD - 16];
        pbw_t a = { list, sizeof(list), 0, false };
        for (uint64_t i = start; i < start + count && i < pb_shared.scan_count; i++) {
            const pb_scan_entry_t *e = &pb_shared.scan[i];
            uint8_t entry[80];
            pbw_t b = { entry, sizeof(entry), 0, false };
            w_str(&b, 1, e->ssid);
            w_uint(&b, 2, e->channel);
            w_int32(&b, 3, e->rssi);
            w_bytes(&b, 4, e->bssid, 6);
            w_uint(&b, 5, e->auth);
            w_bytes(&a, 1, entry, b.len);
        }
        if (a.overflow) {
            w_uint(w, 1, 5);
            w_uint(w, 2, ST_INVALID_ARGUMENT);
            return 0;
        }
        w_uint(w, 1, 5);
        w_bytes(w, 15, list, a.len);
        return 0;
    }
    default:
        w_uint(w, 2, ST_INVALID_PROTO);
        return 0;
    }
}

/* ---- prov-config (network_config.c) ------------------------------------- */

static int handle_config(const uint8_t *in, size_t len, pbw_t *w)
{
    pbf_t f, g;
    bool bad = false;
    uint64_t msg = 0;
    const uint8_t *ssid = NULL, *pass = NULL;
    size_t ssid_len = 0, pass_len = 0;
    FOR_FIELDS(f, in, len, bad) {
        if (f.no == 1 && f.type == 0) msg = f.v;
        if (f.no == 12 && f.type == 2) {
            FOR_FIELDS(g, f.p, f.len, bad) {
                if (g.no == 1 && g.type == 2) { ssid = g.p; ssid_len = g.len; }
                if (g.no == 2 && g.type == 2) { pass = g.p; pass_len = g.len; }
            }
        }
    }
    if (bad) return -1;

    switch (msg) {
    case 0: { /* TypeCmdGetStatus */
        uint8_t body[96];
        pbw_t a = { body, sizeof(body), 0, false };
        w_uint(&a, 2, pb_shared.sta_state);
        if (pb_shared.sta_state == PB_STA_FAILED) {
            /* A oneof member: on the wire even as AuthError (0). */
            w_tag(&a, 10, 0);
            w_varint(&a, pb_shared.sta_reason);
        } else if (pb_shared.sta_state == PB_STA_CONNECTED) {
            /* Delivered once the phone has read it (pairble_pico_ble.cpp):
             * the answer can wait a while behind a busy radio. */
            pb_shared.result_in_response = true;
            uint8_t conn[80];
            pbw_t b = { conn, sizeof(conn), 0, false };
            w_str(&b, 1, pb_shared.ip);
            w_str(&b, 3, pb_shared.connected_ssid);
            w_bytes(&b, 4, pb_shared.connected_bssid, 6);
            w_int32(&b, 5, pb_shared.connected_channel);
            w_bytes(&a, 11, conn, b.len);
        } else if (pb_shared.sta_state == PB_STA_CONNECTING && pb_shared.attempts_remaining) {
            uint8_t att[8];
            pbw_t b = { att, sizeof(att), 0, false };
            w_uint(&b, 1, pb_shared.attempts_remaining);
            w_bytes(&a, 12, att, b.len);
        }
        w_uint(w, 1, 1);
        w_bytes(w, 11, body, a.len);
        return 0;
    }
    case 2: { /* TypeCmdSetConfig */
        uint8_t body[4];
        pbw_t a = { body, sizeof(body), 0, false };
        if (ssid == NULL || ssid_len == 0 || ssid_len > 32 || pass_len > 64) {
            w_uint(&a, 1, ST_INVALID_ARGUMENT);
        } else {
            memcpy(pb_shared.new_ssid, ssid, ssid_len);
            pb_shared.new_ssid[ssid_len] = '\0';
            if (pass_len) memcpy(pb_shared.new_pass, pass, pass_len);
            pb_shared.new_pass[pass_len] = '\0';
        }
        w_uint(w, 1, 3);
        w_bytes(w, 13, body, a.len);
        return 0;
    }
    case 4: { /* TypeCmdApplyConfig */
        uint8_t body[4];
        pbw_t a = { body, sizeof(body), 0, false };
        if (!pb_shared.new_ssid[0]) {
            w_uint(&a, 1, ST_INVALID_ARGUMENT);
        } else if (pb_shared.sta_state == PB_STA_CONNECTING || pb_shared.want_apply) {
            /* One attempt at a time, whoever started it (a sketch's
             * pairble_set_network(), Improv), as /api/connect's 409. */
            w_uint(&a, 1, ST_INTERNAL);
        } else {
            /* Connecting from here on, so a status poll that comes before
             * loop() has started the attempt does not read the old state. */
            pb_shared.sta_state = PB_STA_CONNECTING;
            pb_shared.attempts_remaining = 0;
            pb_shared.want_apply = true;
        }
        w_uint(w, 1, 5);
        w_bytes(w, 15, body, a.len);
        return 0;
    }
    default:
        return -1;
    }
}

/* ---- prov-ctrl (network_ctrl.c) ----------------------------------------- */

static int handle_ctrl(const uint8_t *in, size_t len, pbw_t *w)
{
    pbf_t f;
    bool bad = false;
    uint64_t msg = 0;
    FOR_FIELDS(f, in, len, bad) {
        if (f.no == 1 && f.type == 0) msg = f.v;
    }
    if (bad) return -1;
    if (msg == 1 || msg == 3) {  /* WifiReset, WifiReprov: back to waiting */
        pb_shared.want_reprov = true;
        w_uint(w, 1, msg + 1);
        w_bytes(w, msg == 1 ? 12 : 14, NULL, 0);
        return 0;
    }
    w_uint(w, 1, msg + 1);
    w_uint(w, 2, ST_INVALID_ARGUMENT);
    return 0;
}

/* ---- dispatch ----------------------------------------------------------- */

int pb_proto_handle(int endpoint, const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap, size_t *out_len)
{
    pbw_t w = { out, out_cap, 0, false };
    int rc;

    if (endpoint == EP_VERSION) {
        /* Plain text, before any session (manager.c). */
        size_t n = sizeof(VERSION_JSON) - 1;
        if (n > out_cap) return -1;
        memcpy(out, VERSION_JSON, n);
        *out_len = n;
        return 0;
    }
    if (endpoint == EP_SESSION) {
        rc = handle_session(in, in_len, &w);
        if (rc != 0 || w.overflow) return -1;
        *out_len = w.len;
        return 0;
    }
    /* Everything else rides the session cipher, as protocomm's
     * protocomm_req_handle() does for every endpoint but these two. */
    if (s_session != SESS_READY || in_len > PB_MAX_PAYLOAD) return -1;
    uint8_t plain[PB_MAX_PAYLOAD];
    memcpy(plain, in, in_len);
    ctr_xor(plain, in_len);

    switch (endpoint) {
    case EP_SCAN:   rc = handle_scan(plain, in_len, &w);   break;
    case EP_CONFIG: rc = handle_config(plain, in_len, &w); break;
    case EP_CTRL:   rc = handle_ctrl(plain, in_len, &w);   break;
    default:        rc = -1;
    }
    if (rc != 0 || w.overflow) return -1;
    ctr_xor(out, w.len);
    *out_len = w.len;
    return 0;
}
