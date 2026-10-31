/*
 * pairble for Arduino: one library for the ESP32 (arduino-esp32 3.3+) and the
 * Raspberry Pi Pico W / Pico 2 W (arduino-pico). This picks the header of the
 * board being built for:
 *
 *   pairble_esp32.h    the ESP-IDF component's own header, unchanged
 *   pairble_rp2040.h   the Pico W's, with the same API less what has no
 *                      counterpart there
 *
 * Both have the same events, configuration struct, pairble_start(),
 * pairble_setup_url() and the rest, so one sketch builds for both. Two names
 * bridge what differs: pairble_loop(), which the Pico W needs from loop() and
 * the ESP32 (with a task of its own) ignores, and the PAIRBLE_OK /
 * PAIRBLE_ERR_* return codes, which are esp_err_t codes on the ESP32.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#if defined(ESP_PLATFORM)
#  include "pairble_esp32.h"
#  include "esp_wifi.h"  /* the ESP_ERR_WIFI_* codes below */

typedef esp_err_t pairble_err_t;
#  define PAIRBLE_OK                  ESP_OK
#  define PAIRBLE_ERR_INVALID_ARG     ESP_ERR_INVALID_ARG
#  define PAIRBLE_ERR_INVALID_STATE   ESP_ERR_INVALID_STATE
#  define PAIRBLE_ERR_NO_MEM          ESP_ERR_NO_MEM
#  define PAIRBLE_ERR_WRONG_PASSWORD  ESP_ERR_WIFI_PASSWORD
#  define PAIRBLE_ERR_NOT_FOUND       ESP_ERR_WIFI_SSID
#  define PAIRBLE_ERR_TIMEOUT         ESP_ERR_TIMEOUT

/* pairble runs in its own task on the ESP32: nothing to do. */
static inline void pairble_loop(void) {}

#elif defined(ARDUINO_ARCH_RP2040)
#  if !defined(PICO_CYW43_SUPPORTED)
#    error "pairble needs a board with the CYW43 radio: the Raspberry Pi Pico W, Pico 2 W or similar"
#  else
#    include "pairble_rp2040.h"
#  endif

#else
#  error "pairble supports ESP32 boards (arduino-esp32 3.3 or newer) and the Raspberry Pi Pico W / Pico 2 W (arduino-pico)"
#endif
