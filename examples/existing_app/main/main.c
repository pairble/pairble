/*
 * pairble inside an application that already sets up NVS, networking and the
 * event loop itself, and wants to hear what happens.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * pairble_start() leaves anything already initialised alone, so it can be
 * dropped into an existing app_main without reordering it.
 *
 * It also shows the application's own way back into setup: the lamp's menu
 * (here, simply the log) calls pairble_open_setup(). pairble can watch a
 * button itself instead - CONFIG_PAIRBLE_SETUP_BUTTON_GPIO.
 */

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "pairble.h"

static const char *TAG = "lamp";

static void on_pairble(pairble_event_t event, pairble_fail_reason_t reason, void *ctx)
{
    switch (event) {
    case PAIRBLE_EVENT_STARTED:
        ESP_LOGI(TAG, "waiting for a phone - blink the LED%s",
                 pairble_is_provisioned() ? " (setup reopened, the old network is still tried)" : "");
        break;
    case PAIRBLE_EVENT_FAILED:
        ESP_LOGW(TAG, "%s - the phone says so too",
                 reason == PAIRBLE_FAIL_WRONG_PASSWORD ? "wrong password" : "network not found");
        break;
    case PAIRBLE_EVENT_ONLINE:
        ESP_LOGI(TAG, "online - start the lamp's own work");
        break;
    case PAIRBLE_EVENT_OFFLINE:
        /* pairble keeps retrying, and reopens setup by itself after
         * CONFIG_PAIRBLE_RECOVERY_AFTER_SECONDS; this is for the LED. */
        ESP_LOGW(TAG, "offline: %s", reason == PAIRBLE_FAIL_WRONG_PASSWORD ? "the password changed?"
                                     : reason == PAIRBLE_FAIL_AP_NOT_FOUND ? "router off or out of range"
                                                                           : "connection lost");
        break;
    default:
        break;
    }
}

void app_main(void)
{
    /* What an existing application typically does first. */
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    pairble_config_t config = PAIRBLE_CONFIG_DEFAULT();
    config.name_prefix = "LAMP-";   /* advertises as LAMP-XXXXXX */
    config.event_cb = on_pairble;
    ESP_ERROR_CHECK(pairble_start(&config));

    ESP_LOGI(TAG, "device %s, QR code: %s", pairble_device_name(), pairble_setup_url());
}

/* What the lamp's "Change Wi-Fi network" menu item would call. Keeps the
 * current network until the new one works. */
void lamp_menu_change_network(void)
{
    ESP_ERROR_CHECK(pairble_open_setup());
}
