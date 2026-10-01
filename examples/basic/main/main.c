/*
 * pairble, the smallest integration.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * One call. With credentials stored the device connects; without, it opens
 * setup over BLE and logs the URL to put in a QR code.
 */

#include "esp_log.h"
#include "pairble.h"

void app_main(void)
{
    ESP_ERROR_CHECK(pairble_start(NULL));
    ESP_LOGI("example", "QR code: %s", pairble_setup_url());
}
