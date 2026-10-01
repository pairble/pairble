/* SPDX-License-Identifier: Apache-2.0 */

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "pairble_text.h"

bool pairble_valid_name(const char *name, size_t max)
{
    size_t n = strlen(name);
    if (n == 0 || n > max) {
        return false;
    }
    for (const char *p = name; *p; p++) {
        if (!(isalnum((unsigned char)*p) || *p == '-' || *p == '_')) {
            return false;
        }
    }
    return true;
}

/* URL-safe without escaping, and 8-63 characters because the SoftAP fallback
 * uses it as the WPA2 password (64 would be read as a raw hex key). */
bool pairble_valid_pop(const char *pop)
{
    size_t n = strlen(pop);
    if (n < 8 || n > 63) {
        return false;
    }
    for (const char *p = pop; *p; p++) {
        if (!(isalnum((unsigned char)*p) || strchr("._~-", *p))) {
            return false;
        }
    }
    return true;
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool pairble_parse_uuid(const char *text, uint8_t out[16])
{
    uint8_t msb_first[16];
    int n = 0;
    for (const char *p = text; *p != '\0'; p++) {
        if (*p == '-') {
            continue;
        }
        int hi = hex_digit(p[0]), lo = hi < 0 ? -1 : hex_digit(p[1]);
        if (lo < 0 || n >= 16) {
            return false;
        }
        msb_first[n++] = (uint8_t)(hi << 4 | lo);
        p++;
    }
    if (n != 16) {
        return false;
    }
    for (int i = 0; i < 16; i++) {
        out[i] = msb_first[15 - i];
    }
    return true;
}

/* SSIDs are arbitrary bytes and may contain quotes or control characters. */
void pairble_json_string(char *out, size_t len, const char *in)
{
    size_t o = 0;
    if (len < 3) {
        if (len) out[0] = '\0';
        return;
    }
    out[o++] = '"';
    /* o + 7 < len: room for a six-character escape, the closing quote and NUL. */
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 7 < len; p++) {
        if (*p == '"' || *p == '\\') {
            out[o++] = '\\';
            out[o++] = (char)*p;
        } else if (*p < 0x20) {
            o += snprintf(out + o, len - o, "\\u%04x", *p);
        } else {
            out[o++] = (char)*p;
        }
    }
    out[o++] = '"';
    out[o] = '\0';
}

/* '+' is a space, %XX a byte. A malformed escape or a %00 would otherwise cut
 * the value short without anyone noticing. */
int pairble_url_decode(char *s)
{
    char *w = s;
    for (const char *r = s; *r; r++) {
        if (*r == '+') {
            *w++ = ' ';
        } else if (*r == '%') {
            int hi = hex_digit(r[1]), lo = hi < 0 ? -1 : hex_digit(r[2]);
            if (lo < 0 || (hi | lo) == 0) {
                return -1;
            }
            *w++ = (char)(hi << 4 | lo);
            r += 2;
        } else {
            *w++ = *r;
        }
    }
    *w = '\0';
    return (int)(w - s);
}

/* Additional records (EDNS) are dropped rather than echoed. */
int pairble_dns_answer(uint8_t *buf, int n, size_t cap, uint32_t ip)
{
    if (n < 12 || (buf[2] & 0x80) || buf[4] != 0 || buf[5] != 1) {
        return -1;  /* too short, a response, or not exactly one question */
    }
    int i = 12;
    while (i < n && buf[i] != 0) {
        if (buf[i] & 0xC0) {
            return -1;  /* a compression pointer has no business in a question */
        }
        i += buf[i] + 1;
    }
    if (i + 5 > n) {
        return -1;
    }
    uint16_t qtype = (uint16_t)(buf[i + 1] << 8 | buf[i + 2]);
    int len = i + 5;  /* end of the question */
    bool answer = qtype == 1 && (size_t)len + 16 <= cap;

    buf[2] = 0x84 | (buf[2] & 0x01);  /* response, authoritative, keep RD */
    buf[3] = 0x80;                    /* recursion available, no error */
    buf[6] = 0;
    buf[7] = answer ? 1 : 0;
    buf[8] = buf[9] = buf[10] = buf[11] = 0;
    if (answer) {
        const uint8_t *a = (const uint8_t *)&ip;
        const uint8_t record[16] = { 0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 30, 0, 4, a[0], a[1], a[2], a[3] };
        memcpy(buf + len, record, sizeof(record));
        len += sizeof(record);
    }
    return len;
}
