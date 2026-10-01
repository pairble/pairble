/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * The two ways a person reopens setup without the application writing any
 * code: holding a button, and - for devices with none, a bulb or a plug -
 * switching the power on and off a few times.
 */

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "pairble_internal.h"

static const char *TAG = PAIRBLE_TAG;

/* ---- quick power cycles ------------------------------------------------ */

#if CONFIG_PAIRBLE_POWER_CYCLE_COUNT > 0
static esp_timer_handle_t s_cycle_timer;

static void cycle_window_elapsed(void *arg)
{
    /* On long enough to count as normal use: start counting from zero. */
    pairble_nvs_erase("cycles");
}
#endif

bool pairble_power_cycles_reached(void)
{
#if CONFIG_PAIRBLE_POWER_CYCLE_COUNT > 0
    esp_reset_reason_t why = esp_reset_reason();
    /* A switch flicked quickly can brown the supply out before it cuts it. */
    if (why != ESP_RST_POWERON && why != ESP_RST_BROWNOUT) {
        pairble_nvs_erase("cycles");  /* no flash write if there is nothing to erase */
        return false;
    }
    uint8_t count = 0;
    pairble_nvs_get_blob("cycles", &count, sizeof(count));
    count++;
    if (count >= CONFIG_PAIRBLE_POWER_CYCLE_COUNT) {
        ESP_LOGI(TAG, "switched on %d times in quick succession: opening setup", count);
        pairble_nvs_erase("cycles");
        return true;
    }
    pairble_nvs_set_blob("cycles", &count, sizeof(count));
    const esp_timer_create_args_t args = { .callback = cycle_window_elapsed, .name = "pairble_cycles" };
    if (esp_timer_create(&args, &s_cycle_timer) == ESP_OK) {
        esp_timer_start_once(s_cycle_timer, (uint64_t)CONFIG_PAIRBLE_POWER_CYCLE_SECONDS * 1000 * 1000);
    }
#endif
    return false;
}

/* ---- setup button -------------------------------------------------------- */

#if CONFIG_PAIRBLE_SETUP_BUTTON_GPIO >= 0
#define POLL_MS 50

static esp_timer_handle_t s_button_timer;
static uint32_t s_held_ms;
static bool s_fired;

static void button_poll(void *arg)
{
    int level = gpio_get_level(CONFIG_PAIRBLE_SETUP_BUTTON_GPIO);
#if CONFIG_PAIRBLE_SETUP_BUTTON_ACTIVE_LOW
    bool pressed = level == 0;
#else
    bool pressed = level == 1;
#endif
    if (!pressed) {
        s_held_ms = 0;
        s_fired = false;  /* one trigger per press */
        return;
    }
    s_held_ms += POLL_MS;
    if (!s_fired && s_held_ms >= CONFIG_PAIRBLE_SETUP_BUTTON_HOLD_SECONDS * 1000) {
        s_fired = true;
        ESP_LOGI(TAG, "setup button held for %d s", CONFIG_PAIRBLE_SETUP_BUTTON_HOLD_SECONDS);
        pairble_open_setup();
    }
}
#endif

esp_err_t pairble_button_start(void)
{
#if CONFIG_PAIRBLE_SETUP_BUTTON_GPIO >= 0
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << CONFIG_PAIRBLE_SETUP_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
#if CONFIG_PAIRBLE_SETUP_BUTTON_ACTIVE_LOW
        .pull_up_en = GPIO_PULLUP_ENABLE,
#else
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
#endif
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        return err;
    }
    /* Polled rather than interrupt-driven: a hold of seconds needs a timer
     * anyway, and polling needs no debouncing. */
    const esp_timer_create_args_t args = { .callback = button_poll, .name = "pairble_button" };
    err = esp_timer_create(&args, &s_button_timer);
    if (err == ESP_OK) {
        err = esp_timer_start_periodic(s_button_timer, POLL_MS * 1000);
    }
    return err;
#else
    return ESP_OK;
#endif
}
