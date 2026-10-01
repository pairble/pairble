/*
 * pairble in a PlatformIO project.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * With a network stored the board connects to it; without one it opens setup
 * and logs the URL to put in a QR code. Events go to the log.
 */

#include "esp_log.h"
#include "pairble.h"

static const char *TAG = "example";

static void on_pairble(pairble_event_t event, pairble_fail_reason_t reason, void *ctx)
{
    switch (event) {
    case PAIRBLE_EVENT_STARTED:
        ESP_LOGI(TAG, "setup open - scan the QR code: %s", pairble_setup_url());
        break;
    case PAIRBLE_EVENT_SUCCESS:
        ESP_LOGI(TAG, "the phone set a network that works");
        break;
    case PAIRBLE_EVENT_FAILED:
        ESP_LOGW(TAG, "that did not work (%d), the phone can try again", reason);
        break;
    case PAIRBLE_EVENT_ONLINE:
        ESP_LOGI(TAG, "online");
        break;
    case PAIRBLE_EVENT_OFFLINE:
        ESP_LOGW(TAG, "offline (%d)", reason);
        break;
    default:
        break;
    }
}

void app_main(void)
{
    pairble_config_t config = PAIRBLE_CONFIG_DEFAULT();
    config.event_cb = on_pairble;
    ESP_ERROR_CHECK(pairble_start(&config));
    ESP_LOGI(TAG, "device %s", pairble_device_name());
}
