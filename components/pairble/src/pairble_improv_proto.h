/* SPDX-License-Identifier: Apache-2.0
 *
 * Improv Wi-Fi's serial framing (https://www.improv-wifi.com/serial/), with
 * nothing ESP-specific in it, so it is tested on the host
 * (test/host/test_improv.c): every byte comes from whoever holds the cable.
 *
 * A packet is "IMPROV", version (1), type, data length, data, and a checksum
 * that is the sum of all the bytes before it, modulo 256. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define IMPROV_VERSION 1

enum {
    IMPROV_TYPE_STATE = 0x01,
    IMPROV_TYPE_ERROR = 0x02,
    IMPROV_TYPE_RPC = 0x03,
    IMPROV_TYPE_RPC_RESULT = 0x04,
};

enum {
    IMPROV_STATE_READY = 0x02,
    IMPROV_STATE_PROVISIONING = 0x03,
    IMPROV_STATE_PROVISIONED = 0x04,
};

enum {
    IMPROV_ERROR_NONE = 0x00,
    IMPROV_ERROR_INVALID_RPC = 0x01,
    IMPROV_ERROR_UNKNOWN_RPC = 0x02,
    IMPROV_ERROR_UNABLE_TO_CONNECT = 0x03,
    IMPROV_ERROR_UNKNOWN = 0xFF,
};

enum {
    IMPROV_CMD_WIFI_SETTINGS = 0x01,
    IMPROV_CMD_GET_STATE = 0x02,
    IMPROV_CMD_GET_INFO = 0x03,
    IMPROV_CMD_GET_NETWORKS = 0x04,
};

/* Largest packet: 9 header bytes, 255 data bytes, the checksum. */
#define IMPROV_PACKET_MAX (9 + 255 + 1)

/* A command that arrived whole and with a good checksum. */
typedef struct {
    uint8_t command;
    uint8_t len;
    uint8_t data[255];
} improv_rpc_t;

typedef enum {
    IMPROV_NONE,       /* need more bytes */
    IMPROV_RPC,        /* *rpc holds a command */
    IMPROV_BAD_PACKET, /* a whole packet that is not a valid RPC: answer INVALID_RPC */
} improv_event_t;

typedef struct {
    uint8_t buf[IMPROV_PACKET_MAX];
    size_t len;
} improv_parser_t;

/* Takes one byte. Anything that is not part of a packet - log lines on a
 * shared port, noise - is skipped. */
improv_event_t improv_parse_byte(improv_parser_t *p, uint8_t byte, improv_rpc_t *rpc);

/* Wi-Fi settings: SSID (1-32 bytes) and password (0-63). The outputs are
 * NUL-terminated. False if the lengths do not add up. */
bool improv_wifi_settings(const improv_rpc_t *rpc, char ssid[33], char password[64]);

/* Writes a packet into out (IMPROV_PACKET_MAX bytes); returns its length. */
size_t improv_packet(uint8_t type, const uint8_t *data, size_t len, uint8_t *out);

/* An RPC result answering `command`: n strings, each sent as length + bytes.
 * Returns the packet length, or 0 if the strings do not fit in 255 bytes. */
size_t improv_rpc_result(uint8_t command, const char *const *strings, size_t n, uint8_t *out);
