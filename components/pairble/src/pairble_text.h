/* SPDX-License-Identifier: Apache-2.0
 *
 * The pure functions that handle untrusted input, apart from anything
 * ESP-IDF, so test/host can compile and run them on a computer. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Advertised name: 1-max characters of A-Z a-z 0-9 - _ (a BLE name, a Wi-Fi
 * network name and part of a URL at once). */
bool pairble_valid_name(const char *name, size_t max);

/* Proof of possession: 8-63 characters of A-Z a-z 0-9 . _ ~ - */
bool pairble_valid_pop(const char *pop);

/* "021a9004-0382-..." into the least-significant-byte-first order the BLE
 * stack wants. False if it is not exactly 16 bytes of hex. */
bool pairble_parse_uuid(const char *text, uint8_t out[16]);

/* `in` as a quoted JSON string, escaped; cut short (still valid JSON) if
 * `len` is too small. 6 * strlen(in) + 3 is always enough. */
void pairble_json_string(char *out, size_t len, const char *in);

/* Decodes an application/x-www-form-urlencoded value in place. Returns its
 * length, or -1 for a malformed escape or a %00. */
int pairble_url_decode(char *s);

/* Turns the DNS query in buf[0..n) into the captive portal's answer: an A
 * record with `ip` (network order) for an A query, no answer otherwise.
 * Returns the answer's length, or -1 if it is not a query worth answering. */
int pairble_dns_answer(uint8_t *buf, int n, size_t cap, uint32_t ip);
