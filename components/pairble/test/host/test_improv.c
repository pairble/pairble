/* SPDX-License-Identifier: Apache-2.0
 *
 * Host tests for src/pairble_improv_proto.c: the bytes come from whoever is
 * on the serial port. Run with ./run.sh. */

#include <stdio.h>
#include <string.h>
#include "pairble_improv_proto.h"

static int failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond);       \
            failures++;                                                      \
        }                                                                    \
    } while (0)

/* An RPC command packet the way a client builds it. */
static size_t rpc(uint8_t command, const uint8_t *args, size_t len, uint8_t *out)
{
    uint8_t data[257];
    data[0] = command;
    data[1] = (uint8_t)len;
    memcpy(data + 2, args, len);
    size_t n = improv_packet(IMPROV_TYPE_RPC, data, len + 2, out);
    out[7] = IMPROV_TYPE_RPC;
    return n;
}

static int feed(improv_parser_t *p, const uint8_t *b, size_t n, improv_rpc_t *out, improv_event_t *last)
{
    int events = 0;
    for (size_t i = 0; i < n; i++) {
        improv_event_t e = improv_parse_byte(p, b[i], out);
        if (e != IMPROV_NONE) {
            events++;
            *last = e;
        }
    }
    return events;
}

static void test_packet_layout(void)
{
    uint8_t out[IMPROV_PACKET_MAX];
    uint8_t state = IMPROV_STATE_READY;
    size_t n = improv_packet(IMPROV_TYPE_STATE, &state, 1, out);
    /* "IMPROV" 01 01 01 02 + checksum */
    const uint8_t want[] = { 'I', 'M', 'P', 'R', 'O', 'V', 1, 1, 1, 2, 0 };
    CHECK(n == 11);
    CHECK(memcmp(out, want, 10) == 0);
    uint8_t sum = 0;
    for (int i = 0; i < 10; i++) sum += out[i];
    CHECK(out[10] == sum);
}

static void test_rpc_result(void)
{
    uint8_t out[IMPROV_PACKET_MAX];
    const char *s[] = { "pairble", "1.0", "ESP32-S3", "INK-BA0A68" };
    size_t n = improv_rpc_result(IMPROV_CMD_GET_INFO, s, 4, out);
    CHECK(n == 9 + 2 + (1 + 7) + (1 + 3) + (1 + 8) + (1 + 10) + 1);
    CHECK(out[7] == IMPROV_TYPE_RPC_RESULT && out[9] == IMPROV_CMD_GET_INFO);
    CHECK(out[10] == out[8] - 2);
    CHECK(out[11] == 7 && memcmp(out + 12, "pairble", 7) == 0);
    CHECK(improv_rpc_result(IMPROV_CMD_GET_NETWORKS, NULL, 0, out) == 12);  /* the empty terminator */
    char big[200];
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    const char *two[] = { big, big };
    CHECK(improv_rpc_result(1, two, 2, out) == 0);  /* does not fit: refused, not truncated */
}

static void test_parse_wifi_settings(void)
{
    improv_parser_t p = { 0 };
    improv_rpc_t r;
    improv_event_t e = IMPROV_NONE;
    uint8_t args[] = { 4, 'T', 'e', 's', 't', 10, 'b', 't', 'c', 't', 'e', 's', 't', '1', '2', '3' };
    uint8_t pkt[IMPROV_PACKET_MAX];
    size_t n = rpc(IMPROV_CMD_WIFI_SETTINGS, args, sizeof(args), pkt);
    /* Log text before it, as on a port shared with the console. */
    const char *noise = "I (123) wifi: IMPR IMPROVISE ";
    CHECK(feed(&p, (const uint8_t *)noise, strlen(noise), &r, &e) == 0);
    CHECK(feed(&p, pkt, n, &r, &e) == 1 && e == IMPROV_RPC);
    CHECK(r.command == IMPROV_CMD_WIFI_SETTINGS);
    char ssid[33], pw[64];
    CHECK(improv_wifi_settings(&r, ssid, pw));
    CHECK(strcmp(ssid, "Test") == 0 && strcmp(pw, "btctest123") == 0);

    /* One byte at a time, split anywhere: the same. */
    for (size_t i = 0; i < n; i++) {
        improv_event_t got = improv_parse_byte(&p, pkt[i], &r);
        CHECK(got == (i == n - 1 ? IMPROV_RPC : IMPROV_NONE));
    }
}

static void test_bad_packets(void)
{
    improv_parser_t p = { 0 };
    improv_rpc_t r;
    improv_event_t e = IMPROV_NONE;
    uint8_t pkt[IMPROV_PACKET_MAX];
    size_t n = rpc(IMPROV_CMD_GET_STATE, NULL, 0, pkt);

    pkt[n - 1] ^= 1;  /* checksum */
    CHECK(feed(&p, pkt, n, &r, &e) == 1 && e == IMPROV_BAD_PACKET);
    pkt[n - 1] ^= 1;
    CHECK(feed(&p, pkt, n, &r, &e) == 1 && e == IMPROV_RPC);  /* and the parser recovered */

    pkt[6] = 2;  /* another version: not ours, skipped like any other text */
    CHECK(feed(&p, pkt, n, &r, &e) == 0);
    pkt[6] = 1;
    CHECK(feed(&p, pkt, n, &r, &e) == 1 && e == IMPROV_RPC);

    /* An RPC whose inner length disagrees with the packet's. */
    uint8_t data[] = { IMPROV_CMD_GET_STATE, 5 };
    n = improv_packet(IMPROV_TYPE_RPC, data, sizeof(data), pkt);
    CHECK(feed(&p, pkt, n, &r, &e) == 1 && e == IMPROV_BAD_PACKET);

    /* Wi-Fi settings whose lengths lie. */
    improv_rpc_t w = { .command = IMPROV_CMD_WIFI_SETTINGS };
    char ssid[33], pw[64];
    const uint8_t lies[][6] = {
        { 0, 0 },                     /* empty SSID */
        { 33 },                       /* longer than the data */
        { 1, 'a', 9, 'x' },           /* password longer than the data */
        { 1, 'a', 0, 'x' },           /* bytes left over */
    };
    const uint8_t lens[] = { 2, 1, 4, 4 };
    for (size_t i = 0; i < sizeof(lens); i++) {
        w.len = lens[i];
        memcpy(w.data, lies[i], lens[i]);
        CHECK(!improv_wifi_settings(&w, ssid, pw));
    }
    w.len = 2 + 32 + 64;  /* a 64-byte password */
    memset(w.data, 'a', w.len);
    w.data[0] = 32;
    w.data[33] = 64;
    CHECK(!improv_wifi_settings(&w, ssid, pw));
}

int main(void)
{
    test_packet_layout();
    test_rpc_result();
    test_parse_wifi_settings();
    test_bad_packets();
    if (failures) {
        fprintf(stderr, "pairble_improv: %d check(s) failed\n", failures);
        return 1;
    }
    printf("pairble_improv: all checks passed\n");
    return 0;
}
