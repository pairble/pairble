/* SPDX-License-Identifier: Apache-2.0 */
#include "pairble_improv_proto.h"

#include <string.h>

static const uint8_t HEADER[6] = { 'I', 'M', 'P', 'R', 'O', 'V' };

static uint8_t checksum(const uint8_t *b, size_t len)
{
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum += b[i];
    }
    return sum;
}

improv_event_t improv_parse_byte(improv_parser_t *p, uint8_t byte, improv_rpc_t *rpc)
{
    if (p->len < sizeof(HEADER)) {
        if (byte == HEADER[p->len]) {
            p->buf[p->len++] = byte;
        } else {
            /* Not where a packet starts; this byte may start one. */
            p->len = byte == HEADER[0] ? 1 : 0;
            p->buf[0] = byte;
        }
        return IMPROV_NONE;
    }
    if (p->len == sizeof(HEADER) && byte != IMPROV_VERSION) {
        /* "IMPROV" in a log line ("IMPROVISED"), not a packet: look again. */
        p->len = byte == HEADER[0] ? 1 : 0;
        p->buf[0] = byte;
        return IMPROV_NONE;
    }
    p->buf[p->len++] = byte;
    if (p->len < 9) {
        return IMPROV_NONE;
    }
    size_t whole = 9 + (size_t)p->buf[8] + 1;
    if (p->len < whole) {
        return IMPROV_NONE;
    }
    p->len = 0;  /* the next byte starts over, whatever this packet was */

    const uint8_t *b = p->buf;
    uint8_t data_len = b[8];
    if (b[6] != IMPROV_VERSION || b[7] != IMPROV_TYPE_RPC || checksum(b, whole - 1) != b[whole - 1]) {
        return IMPROV_BAD_PACKET;
    }
    /* RPC data: command, length of what follows, that many bytes. */
    if (data_len < 2 || b[10] != data_len - 2) {
        return IMPROV_BAD_PACKET;
    }
    rpc->command = b[9];
    rpc->len = b[10];
    memcpy(rpc->data, b + 11, rpc->len);
    return IMPROV_RPC;
}

bool improv_wifi_settings(const improv_rpc_t *rpc, char ssid[33], char password[64])
{
    if (rpc->len < 2) {
        return false;
    }
    size_t ssid_len = rpc->data[0];
    if (ssid_len < 1 || ssid_len > 32 || 1 + ssid_len + 1 > rpc->len) {
        return false;
    }
    size_t pass_len = rpc->data[1 + ssid_len];
    if (pass_len > 63 || 1 + ssid_len + 1 + pass_len != rpc->len) {
        return false;
    }
    memcpy(ssid, rpc->data + 1, ssid_len);
    ssid[ssid_len] = '\0';
    memcpy(password, rpc->data + 2 + ssid_len, pass_len);
    password[pass_len] = '\0';
    return true;
}

size_t improv_packet(uint8_t type, const uint8_t *data, size_t len, uint8_t *out)
{
    if (len > 255) {
        return 0;
    }
    memcpy(out, HEADER, sizeof(HEADER));
    out[6] = IMPROV_VERSION;
    out[7] = type;
    out[8] = (uint8_t)len;
    if (len > 0) {
        memcpy(out + 9, data, len);
    }
    out[9 + len] = checksum(out, 9 + len);
    return 9 + len + 1;
}

size_t improv_rpc_result(uint8_t command, const char *const *strings, size_t n, uint8_t *out)
{
    uint8_t data[255];
    size_t o = 2;
    for (size_t i = 0; i < n; i++) {
        size_t l = strlen(strings[i]);
        if (l > 255 || o + 1 + l > sizeof(data)) {
            return 0;
        }
        data[o++] = (uint8_t)l;
        memcpy(data + o, strings[i], l);
        o += l;
    }
    data[0] = command;
    data[1] = (uint8_t)(o - 2);
    return improv_packet(IMPROV_TYPE_RPC_RESULT, data, o, out);
}
