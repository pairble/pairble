/* SPDX-License-Identifier: Apache-2.0
 *
 * Host tests for src/pairble_text.c: everything there reads input an
 * attacker in radio range controls. Run with ./run.sh. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pairble_text.h"

static int failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond);       \
            failures++;                                                      \
        }                                                                    \
    } while (0)

static int decode(const char *in, char *out)
{
    strcpy(out, in);
    return pairble_url_decode(out);
}

static void test_url_decode(void)
{
    char s[64];
    CHECK(decode("a+b%20c", s) == 5 && strcmp(s, "a b c") == 0);
    CHECK(decode("%C5%A1", s) == 2 && strcmp(s, "\xC5\xA1") == 0);   /* UTF-8 */
    CHECK(decode("%2b%2B", s) == 2 && strcmp(s, "++") == 0);         /* either case */
    CHECK(decode("", s) == 0);
    CHECK(decode("abc%", s) == -1);    /* truncated escape */
    CHECK(decode("abc%4", s) == -1);
    CHECK(decode("%zz", s) == -1);     /* not hex: used to become a NUL */
    CHECK(decode("a%00b", s) == -1);   /* would cut the value short */
    CHECK(decode("%4g", s) == -1);
}

static void test_json_string(void)
{
    char out[256];
    pairble_json_string(out, sizeof(out), "plain");
    CHECK(strcmp(out, "\"plain\"") == 0);
    pairble_json_string(out, sizeof(out), "a\"b\\c");
    CHECK(strcmp(out, "\"a\\\"b\\\\c\"") == 0);
    pairble_json_string(out, sizeof(out), "\x01\x1f");
    CHECK(strcmp(out, "\"\\u0001\\u001f\"") == 0);

    /* The worst 32-byte SSID fits the buffer api_scan gives it. */
    char worst[33];
    memset(worst, 0x01, 32);
    worst[32] = '\0';
    char fits[6 * 32 + 3];
    pairble_json_string(fits, sizeof(fits), worst);
    CHECK(strlen(fits) == 6 * 32 + 2 && fits[strlen(fits) - 1] == '"');

    /* Too small: cut short, still a closed string, never past the end. */
    for (size_t len = 0; len < 20; len++) {
        char *small = malloc(len ? len : 1);
        pairble_json_string(small, len, "\"\"\"\x01\x01\x01xxxxxxxx");
        if (len >= 3) {
            CHECK(strlen(small) < len && small[0] == '"' && small[strlen(small) - 1] == '"');
        }
        free(small);
    }
}

static void test_valid(void)
{
    CHECK(pairble_valid_name("PAIRBLE-BA6550", 29));
    CHECK(pairble_valid_name("a_b", 29));
    CHECK(!pairble_valid_name("", 29));
    CHECK(!pairble_valid_name("has space", 29));
    CHECK(!pairble_valid_name("x&pop=1", 29));
    CHECK(!pairble_valid_name("123456789012345678901234567890", 29));

    CHECK(pairble_valid_pop("XG9S6W639ZAB"));
    CHECK(pairble_valid_pop("a.b_c~d-"));
    CHECK(!pairble_valid_pop("short77"));
    CHECK(!pairble_valid_pop("has space1"));
    CHECK(!pairble_valid_pop("amp&ersand"));
    char pop[65];
    memset(pop, 'A', 63);
    pop[63] = '\0';
    CHECK(pairble_valid_pop(pop));
    pop[63] = 'A';
    pop[64] = '\0';
    CHECK(!pairble_valid_pop(pop));   /* 64 would be taken for a hex PSK */
}

static void test_uuid(void)
{
    uint8_t out[16];
    CHECK(pairble_parse_uuid("021a9004-0382-4aea-bff4-6b3f1c5adfb4", out));
    CHECK(out[0] == 0xb4 && out[15] == 0x02);   /* least significant byte first */
    CHECK(!pairble_parse_uuid("021a9004-0382-4aea-bff4-6b3f1c5adfb", out));    /* odd */
    CHECK(!pairble_parse_uuid("021a9004-0382-4aea-bff4-6b3f1c5adfb4ff", out)); /* 17 */
    CHECK(!pairble_parse_uuid("021a9004-0382-4aea-bff4-6b3f1c5adfzz", out));
    CHECK(!pairble_parse_uuid("", out));
}

/* A query for `name` (as DNS labels) with type `qtype`. */
static int query(uint8_t *buf, const char *labels, size_t labels_len, uint16_t qtype)
{
    const uint8_t header[12] = { 0x12, 0x34, 0x01, 0x00, 0, 1, 0, 0, 0, 0, 0, 1 };
    memcpy(buf, header, 12);
    memcpy(buf + 12, labels, labels_len);
    int i = 12 + (int)labels_len;
    buf[i++] = (uint8_t)(qtype >> 8);
    buf[i++] = (uint8_t)qtype;
    buf[i++] = 0;
    buf[i++] = 1;
    return i;
}

static void test_dns(void)
{
    uint8_t buf[512];
    const uint32_t ip = 0x0104A8C0;  /* 192.168.4.1 in network order */
    const char name[] = "\x05" "apple\x03" "com";   /* sizeof includes the root label */

    int n = query(buf, name, sizeof(name), 1);
    int len = pairble_dns_answer(buf, n, sizeof(buf), ip);
    CHECK(len == n + 16);
    CHECK(buf[0] == 0x12 && buf[1] == 0x34);            /* same id */
    CHECK((buf[2] & 0x80) && buf[7] == 1);              /* a response, one answer */
    CHECK(buf[11] == 0);                                 /* EDNS record dropped */
    CHECK(memcmp(buf + len - 4, "\xC0\xA8\x04\x01", 4) == 0);

    n = query(buf, name, sizeof(name), 28);             /* AAAA: no answer */
    len = pairble_dns_answer(buf, n, sizeof(buf), ip);
    CHECK(len == n && buf[7] == 0);

    n = query(buf, name, sizeof(name), 1);             /* no room: no answer claimed */
    len = pairble_dns_answer(buf, n, (size_t)n + 15, ip);
    CHECK(len == n && buf[7] == 0);

    n = query(buf, name, sizeof(name), 1);
    buf[2] |= 0x80;                                      /* a response: ignored */
    CHECK(pairble_dns_answer(buf, n, sizeof(buf), ip) == -1);

    n = query(buf, name, sizeof(name), 1);
    CHECK(pairble_dns_answer(buf, 11, sizeof(buf), ip) == -1);     /* short */
    CHECK(pairble_dns_answer(buf, n - 1, sizeof(buf), ip) == -1);  /* question cut */
    CHECK(pairble_dns_answer(buf, -1, sizeof(buf), ip) == -1);     /* recvfrom error */

    const char pointer[] = "\xC0\x0C";
    n = query(buf, pointer, sizeof(pointer), 1);
    CHECK(pairble_dns_answer(buf, n, sizeof(buf), ip) == -1);

    /* A label length that points past the end must not be followed. */
    const char runaway[] = "\x3f" "ab";
    n = query(buf, runaway, sizeof(runaway), 1);
    CHECK(pairble_dns_answer(buf, n, sizeof(buf), ip) == -1);

    /* Random input never reads or writes out of bounds (the sanitizers
     * would say so) and never answers with more than fits. */
    srand(1);
    for (int round = 0; round < 20000; round++) {
        int len_in = rand() % 80;
        uint8_t *fuzz = malloc(128);
        for (int i = 0; i < len_in; i++) fuzz[i] = (uint8_t)rand();
        if (len_in > 5) { fuzz[2] &= 0x7f; fuzz[4] = 0; fuzz[5] = 1; }
        int out = pairble_dns_answer(fuzz, len_in, 128, ip);
        CHECK(out <= 128);
        free(fuzz);
    }
}

int main(void)
{
    test_url_decode();
    test_json_string();
    test_valid();
    test_uuid();
    test_dns();
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("pairble_text: all checks passed\n");
    return 0;
}
