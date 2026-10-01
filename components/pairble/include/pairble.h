/*
 * pairble - Wi-Fi onboarding for ESP32 over BLE, with nothing to install
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The whole integration:
 *
 *     ESP_ERROR_CHECK(pairble_start(NULL));
 *     ESP_LOGI(TAG, "setup: %s", pairble_setup_url());
 *
 * pairble_start() connects with stored credentials if there are any and
 * otherwise opens setup; pairble_setup_url() is what goes into the QR code.
 *
 * When the stored network stops working, setup reopens by itself after
 * CONFIG_PAIRBLE_RECOVERY_AFTER_SECONDS offline, on a long press of
 * CONFIG_PAIRBLE_SETUP_BUTTON_GPIO, after CONFIG_PAIRBLE_POWER_CYCLE_COUNT
 * quick power cycles, or when the application calls pairble_open_setup().
 * None of these erases anything: the stored network keeps being retried and
 * is replaced only once the device has joined a new one.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "sdkconfig.h"

/*
 * BLE is mandatory (design decision R1). A target without Bluetooth must fail
 * here rather than silently degrade to a SoftAP-only product.
 *
 * The check is on the SoC capability, not on a hand-maintained chip list:
 * ESP32-C2 does have BLE 5.3 despite what some third-party libraries claim,
 * and ESP32-S2 is the only current target with no Bluetooth at all.
 */
#include "soc/soc_caps.h"
#if !SOC_BLE_SUPPORTED
#  error "pairble: this target has no Bluetooth LE. Supported: ESP32, C2, C3, C5, C6, S3. ESP32-S2 is not supported."
#endif
#if !defined(CONFIG_BT_ENABLED)
#  error "pairble: enable Bluetooth in menuconfig (Component config > Bluetooth)."
#endif

#ifdef __cplusplus
extern "C" {
#endif

/** Longest name BLE advertising allows in ESP-IDF (MAX_BLE_DEVNAME_LEN). */
#define PAIRBLE_NAME_MAX_LEN 29

typedef enum {
    PAIRBLE_EVENT_STARTED,              /* setup open, waiting for a phone; with
                                           pairble_is_provisioned() true it was
                                           reopened and the old network is kept */
    PAIRBLE_EVENT_CREDENTIALS_RECEIVED, /* a phone sent a network to try         */
    PAIRBLE_EVENT_SUCCESS,              /* joined it; the phone has been told    */
    PAIRBLE_EVENT_FAILED,               /* a phone's network did not work: wrong
                                           password, not found, or connection
                                           failed; the phone may try again        */
    PAIRBLE_EVENT_STOPPED,              /* setup torn down, RAM released         */
    PAIRBLE_EVENT_ONLINE,               /* has an IP, provisioned or not         */
    PAIRBLE_EVENT_OFFLINE,              /* lost or cannot reach the stored network;
                                           reason says why. Retried with backoff;
                                           emitted again only if the reason changes.
                                           Also when manually opened setup lets go
                                           of the network for a phone            */
} pairble_event_t;

typedef enum {
    PAIRBLE_FAIL_NONE = 0,
    PAIRBLE_FAIL_WRONG_PASSWORD,
    PAIRBLE_FAIL_AP_NOT_FOUND,
    PAIRBLE_FAIL_CONNECTION_LOST,       /* anything else: weak signal, an AP whose
                                           security the device will not use      */
} pairble_fail_reason_t;

/* Called from the default event loop task. Keep it short. */
typedef void (*pairble_event_cb_t)(pairble_event_t event,
                                  pairble_fail_reason_t reason,
                                  void *user_ctx);

/* Every field is optional: pairble_start(NULL) uses the defaults. Start from
 * PAIRBLE_CONFIG_DEFAULT() rather than a zeroed struct, in which softap,
 * reboot_after_success and recovery_after_seconds are all off. */
typedef struct {
    /* The advertised name, exactly. NULL (the default) builds one from
     * `name_prefix` and the last three bytes of the MAC, so every unit is
     * distinct - which matters, because the iPhone app finds the device by
     * exact name and would otherwise find a neighbour's. Set a fixed name only
     * for one-off devices. At most PAIRBLE_NAME_MAX_LEN characters of A-Z,
     * a-z, 0-9, '-' and '_': it is also a Wi-Fi network name and part of a URL. */
    const char *name;

    /* NULL = CONFIG_PAIRBLE_NAME_PREFIX ("PAIRBLE-"). At most 23 characters,
     * leaving room for the six-character MAC suffix. */
    const char *name_prefix;

    /* Proof of possession: the secret on the label, mixed into the session key.
     * NULL (the default) reads it from NVS, where @pairble/cli puts a unique one
     * per unit, and if there is none generates one on first boot and stores it
     * (unless CONFIG_PAIRBLE_POP_GENERATE is off). A string set here is the same
     * on every unit built from this firmware: fine for a one-off, not for a
     * product. 8-63 characters of A-Z, a-z, 0-9 and ._~- */
    const char *pop;

    /* Also offer setup over the device's own Wi-Fi network, at the same time
     * as BLE, for phones that cannot use Bluetooth. Its name is the device
     * name and its WPA2 password the PoP. Default CONFIG_PAIRBLE_SOFTAP. */
    bool softap;

    uint32_t window_seconds;   /* first-time setup; 0 = stay open until provisioned */
    bool reboot_after_success;

    /* Offline this long, setup reopens by itself; 0 = never. Default
     * CONFIG_PAIRBLE_RECOVERY_AFTER_SECONDS (120). */
    uint32_t recovery_after_seconds;
    /* How long reopened setup stays open. Default
     * CONFIG_PAIRBLE_RECOVERY_WINDOW_SECONDS (600). */
    uint32_t recovery_window_seconds;

    pairble_event_cb_t event_cb;
    void *user_ctx;
} pairble_config_t;

#ifdef CONFIG_PAIRBLE_SOFTAP
#  define PAIRBLE_DEFAULT_SOFTAP true
#else
#  define PAIRBLE_DEFAULT_SOFTAP false
#endif

#ifdef CONFIG_PAIRBLE_REBOOT_AFTER_SUCCESS
#  define PAIRBLE_DEFAULT_REBOOT true
#else
#  define PAIRBLE_DEFAULT_REBOOT false
#endif

#define PAIRBLE_CONFIG_DEFAULT()                                  \
    {                                                             \
        .name                 = NULL,                             \
        .name_prefix          = NULL,                             \
        .pop                  = NULL,                             \
        .softap               = PAIRBLE_DEFAULT_SOFTAP,          \
        .window_seconds       = CONFIG_PAIRBLE_WINDOW_SECONDS,   \
        .reboot_after_success = PAIRBLE_DEFAULT_REBOOT,          \
        .recovery_after_seconds  = CONFIG_PAIRBLE_RECOVERY_AFTER_SECONDS,  \
        .recovery_window_seconds = CONFIG_PAIRBLE_RECOVERY_WINDOW_SECONDS, \
        .event_cb             = NULL,                            \
        .user_ctx             = NULL,                            \
    }

/**
 * Brings the device online: connects with stored credentials if there are
 * any, otherwise opens setup over BLE. `config` may be NULL.
 *
 * Takes care of NVS, networking, the default event loop and Wi-Fi if the
 * application has not already, and leaves them alone if it has - so it works
 * both as the first line of app_main and inside an existing application.
 * Call it before the application starts Wi-Fi or Bluetooth itself: from
 * then on pairble owns the station connection and reconnects it whenever it
 * drops, an esp_wifi_disconnect() by the application included. To switch
 * Wi-Fi off on purpose - before deep sleep, say - call esp_wifi_stop(): that
 * is not counted as time offline, so a battery device that sleeps between
 * readings never reopens setup because of its own sleep. Time offline does
 * add up across wake-ups, but setup only reopens during one that lasts at
 * least ten seconds: a device that sleeps again sooner should stay awake
 * for a while when it has been offline long (or call pairble_open_setup()).
 *
 * First-time setup that closes after window_seconds with nothing set leaves
 * the device idle until the button, power cycles or pairble_open_setup().
 */
esp_err_t pairble_start(const pairble_config_t *config);

/** What goes into the QR code: the setup page, with this unit's name and
 *  proof of possession in the URL fragment, which never reaches a server.
 *  Valid after pairble_start(). Print it, or draw it on a display. */
const char *pairble_setup_url(void);

/** The second, smaller QR code for the SoftAP fallback: scanning it joins the
 *  device's own network, e.g. "WIFI:S:PAIRBLE-BA6550;T:WPA;P:K3F9Q2M7ZX4P;;".
 *  NULL when the fallback is off. Valid after pairble_start(). */
const char *pairble_wifi_qr(void);

/** The advertised name, e.g. "PAIRBLE-BA6550". Valid after pairble_start(). */
const char *pairble_device_name(void);

/** True if Wi-Fi credentials are stored. Valid after pairble_start(). */
bool pairble_is_provisioned(void);

/** True while setup is open (BLE advertising, SoftAP up). A battery device
 *  should not go to deep sleep while it is. */
bool pairble_is_setup_open(void);

/** Opens setup again on a provisioned device, for the application's own
 *  button or menu ("change Wi-Fi network"). The device stays on the stored
 *  network until a phone connects to set it up, falls back to it if the
 *  phone leaves or the new network fails, and replaces it only once the new
 *  one works. Setup closes when a phone has set a network, or after
 *  recovery_window_seconds.
 *
 *  (Setup that reopens by itself after CONFIG_PAIRBLE_RECOVERY_AFTER_SECONDS
 *  offline differs in one way: it also closes as soon as the stored network
 *  answers again.)
 *
 *  Returns at once; ESP_OK if setup is already open. With
 *  CONFIG_PAIRBLE_BT_MEMORY_RELEASE_ALL this restarts the device into setup
 *  instead, and does not return. */
esp_err_t pairble_open_setup(void);

/** Closes setup early and frees its RAM. */
esp_err_t pairble_stop(void);

/** Factory reset for Wi-Fi: wipes the stored network (not the proof of
 *  possession). Restart afterwards. To let someone change networks, prefer
 *  pairble_open_setup(), which erases nothing until the new one works. */
esp_err_t pairble_reset_credentials(void);

#ifdef __cplusplus
}
#endif
