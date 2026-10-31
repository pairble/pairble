/*
 * pairble for the Raspberry Pi Pico W and Pico 2 W (arduino-pico)
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The same setup as on an ESP32 - one QR code, the phone's camera, the App
 * Clip or the web page - and the same protocol on the wire, so pairble's
 * clients cannot tell the two apart. The API is the ESP32's, less what has
 * no counterpart here; a sketch written against this header builds for both:
 *
 *     void setup() { pairble_start(NULL); Serial.println(pairble_setup_url()); }
 *     void loop()  { pairble_loop(); }
 *
 * pairble_start() joins the stored network if there is one and otherwise
 * opens setup. Unlike on the ESP32 there is no task of its own to run the
 * Wi-Fi side, so pairble_loop() has to be called from loop() (on an ESP32 it
 * does nothing).
 *
 * While setup is open the board also offers its own WPA2 network, named as
 * it advertises and with the proof of possession as the password, serving
 * the setup page at 192.168.4.1 for phones without Bluetooth.
 *
 * Board: Raspberry Pi Pico W or Pico 2 W, with Tools > IP/Bluetooth Stack set
 * to one of the "+ Bluetooth" options (PlatformIO:
 * -DPIO_FRAMEWORK_ARDUINO_ENABLE_BLUETOOTH). Settings, as on the ESP32, are
 * build flags: CONFIG_PAIRBLE_NAME_PREFIX, CONFIG_PAIRBLE_SERVICE_UUID,
 * CONFIG_PAIRBLE_SETUP_URL, CONFIG_PAIRBLE_SOFTAP (0 leaves the access point
 * out of the build), CONFIG_PAIRBLE_WINDOW_SECONDS,
 * CONFIG_PAIRBLE_RECOVERY_AFTER_SECONDS, CONFIG_PAIRBLE_RECOVERY_WINDOW_SECONDS,
 * CONFIG_PAIRBLE_REBOOT_AFTER_SUCCESS.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if !defined(ENABLE_BLE)
#  error "pairble: set Tools > IP/Bluetooth Stack to an option with Bluetooth (\"IPv4 + Bluetooth\")"
#endif

#ifndef CONFIG_PAIRBLE_SOFTAP
#define CONFIG_PAIRBLE_SOFTAP 1
#endif
#ifndef CONFIG_PAIRBLE_SERVICE_UUID
#define CONFIG_PAIRBLE_SERVICE_UUID "021a9004-0382-4aea-bff4-6b3f1c5adfb4"
#endif
#ifndef CONFIG_PAIRBLE_NAME_PREFIX
#define CONFIG_PAIRBLE_NAME_PREFIX "PAIRBLE-"
#endif
#ifndef CONFIG_PAIRBLE_SETUP_URL
#define CONFIG_PAIRBLE_SETUP_URL "https://pairble.com/s"
#endif
#ifndef CONFIG_PAIRBLE_WINDOW_SECONDS
#define CONFIG_PAIRBLE_WINDOW_SECONDS 0
#endif
#ifndef CONFIG_PAIRBLE_RECOVERY_AFTER_SECONDS
#define CONFIG_PAIRBLE_RECOVERY_AFTER_SECONDS 120
#endif
#ifndef CONFIG_PAIRBLE_RECOVERY_WINDOW_SECONDS
#define CONFIG_PAIRBLE_RECOVERY_WINDOW_SECONDS 600
#endif
/* Off here, unlike on the ESP32: closing setup gives its RAM back without
 * one. */
#ifndef CONFIG_PAIRBLE_REBOOT_AFTER_SUCCESS
#define CONFIG_PAIRBLE_REBOOT_AFTER_SUCCESS 0
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* What the functions below return: 0 on success, as ESP_OK. The same names
 * stand for the ESP32's esp_err_t codes there. */
typedef int pairble_err_t;
#define PAIRBLE_OK                  0
#define PAIRBLE_ERR_INVALID_ARG     (-1)
#define PAIRBLE_ERR_INVALID_STATE   (-2)
#define PAIRBLE_ERR_NO_MEM          (-3)
#define PAIRBLE_ERR_WRONG_PASSWORD  (-4)
#define PAIRBLE_ERR_NOT_FOUND       (-5)  /* the network did not answer */
#define PAIRBLE_ERR_TIMEOUT         (-6)

#define PAIRBLE_NAME_MAX_LEN 29

/* As on the ESP32 (components/pairble/include/pairble.h). */
typedef enum {
    PAIRBLE_EVENT_STARTED,              /* setup open, waiting for a phone       */
    PAIRBLE_EVENT_CREDENTIALS_RECEIVED, /* a phone sent a network to try         */
    PAIRBLE_EVENT_SUCCESS,              /* joined it; the phone has been told    */
    PAIRBLE_EVENT_FAILED,               /* a phone's network did not work        */
    PAIRBLE_EVENT_STOPPED,              /* setup closed, RAM released            */
    PAIRBLE_EVENT_ONLINE,               /* has an IP                             */
    PAIRBLE_EVENT_OFFLINE,              /* lost or cannot reach the stored network */
} pairble_event_t;

typedef enum {
    PAIRBLE_FAIL_NONE = 0,
    PAIRBLE_FAIL_WRONG_PASSWORD,
    PAIRBLE_FAIL_AP_NOT_FOUND,
    PAIRBLE_FAIL_CONNECTION_LOST,
} pairble_fail_reason_t;

/* Called from pairble_loop(), never from an interrupt. */
typedef void (*pairble_event_cb_t)(pairble_event_t event, pairble_fail_reason_t reason, void *ctx);

/* The ESP32's fields, in its order; see its header for each. */
typedef struct {
    const char *name;             /* full advertised name; NULL: prefix + MAC   */
    const char *name_prefix;      /* NULL: CONFIG_PAIRBLE_NAME_PREFIX            */
    const char *pop;              /* NULL: generated once and kept in flash      */
    bool softap;                  /* also the board's own network; needs
                                     CONFIG_PAIRBLE_SOFTAP in the build         */
    uint32_t window_seconds;      /* first-time setup; 0: open until one is set */
    bool reboot_after_success;
    uint32_t recovery_after_seconds;  /* reopen setup after this long offline;
                                         0: never                               */
    uint32_t recovery_window_seconds; /* how long reopened setup stays open     */
    pairble_event_cb_t event_cb;
    void *user_ctx;
} pairble_config_t;

#define PAIRBLE_CONFIG_DEFAULT()                                            \
    {                                                                       \
        NULL, NULL, NULL, CONFIG_PAIRBLE_SOFTAP != 0,                       \
        CONFIG_PAIRBLE_WINDOW_SECONDS, CONFIG_PAIRBLE_REBOOT_AFTER_SUCCESS != 0, \
        CONFIG_PAIRBLE_RECOVERY_AFTER_SECONDS, CONFIG_PAIRBLE_RECOVERY_WINDOW_SECONDS, \
        NULL, NULL                                                          \
    }

/* The reason goes to Serial when it is not PAIRBLE_OK. */
pairble_err_t pairble_start(const pairble_config_t *config);
void pairble_loop(void);

const char *pairble_setup_url(void);    /* what goes into the QR code */
const char *pairble_wifi_qr(void);      /* "WIFI:S:...;T:WPA;P:...;;", NULL without SoftAP */
const char *pairble_device_name(void);
bool pairble_is_provisioned(void);      /* a network is stored */
bool pairble_is_setup_open(void);
bool pairble_is_online(void);

/* Opens setup without forgetting the stored network, which keeps being used
 * until a phone sets one that works; closes again after
 * recovery_window_seconds. PAIRBLE_OK if it is open already. */
pairble_err_t pairble_open_setup(void);

/* A network from elsewhere - a keypad, a menu - taken as one from a phone.
 * Setup must be open. timeout_ms 0 returns at once; otherwise it waits for
 * the outcome (running pairble_loop() meanwhile, so not from an event
 * callback): PAIRBLE_OK, PAIRBLE_ERR_WRONG_PASSWORD, PAIRBLE_ERR_NOT_FOUND or
 * PAIRBLE_ERR_TIMEOUT. PAIRBLE_ERR_INVALID_STATE: setup is not open, or an
 * attempt is running already. */
pairble_err_t pairble_set_network(const char *ssid, const char *password, uint32_t timeout_ms);

/* Improv Wi-Fi over serial (https://www.improv-wifi.com/serial/), as on the
 * ESP32: the ESP Web Tools installer, improv-wifi.com and Home Assistant can
 * then set the network over the cable. Bytes from the port go to
 * pairble_improv_input() (log lines on the same port are fine), answers to
 * `write`, from pairble_loop(). `url`, if given, is offered once the board is
 * online, "{ip}" replaced by its address. */
typedef void (*pairble_improv_write_t)(const uint8_t *data, size_t len, void *ctx);
pairble_err_t pairble_improv_start(pairble_improv_write_t write, void *ctx, const char *url);
void pairble_improv_set_firmware(const char *name, const char *version);
void pairble_improv_input(const uint8_t *data, size_t len);
/* Here only: true while the last byte given was part of an Improv packet,
 * for a sketch that reads the port for its own commands too. */
bool pairble_improv_parsing(void);

/* Closes setup early and frees its RAM. */
pairble_err_t pairble_stop(void);

/* Wipes the stored network (not the proof of possession). Restart
 * afterwards, or call pairble_open_setup(). */
pairble_err_t pairble_reset_credentials(void);

#ifdef __cplusplus
}

class Stream;
/* Improv over this serial port, which pairble_loop() then reads. */
void pairbleImprovSerial(Stream &serial, const char *url = nullptr, const char *firmware = nullptr,
                         const char *version = nullptr);
#endif
