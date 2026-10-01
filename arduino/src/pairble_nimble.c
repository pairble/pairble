/*
 * pairble for Arduino: starting and stopping NimBLE-Arduino's host.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * In ESP-IDF, nimble_port_init() brings up the Bluetooth controller too;
 * NimBLE-Arduino leaves that to its C++ NimBLEDevice::init(). protocomm's
 * transport (pairble_ble_nimble.c) calls these instead, which do the same as
 * NimBLEDevice::init() and deinit() for the parts protocomm does not.
 *
 * Stopping is ours too. NimBLE-Arduino's nimble_port_stop() waits on a
 * counting semaphore that the host-stop callback can release twice (a phone
 * reconnecting while setup closes) and return while the host task still
 * runs; and its host deinitialises the host timer while stopping even when
 * the timer's event is already queued. Either way the task ends up calling a
 * NULL callback. Here the host task runs its own loop, which skips such an
 * event, and stop returns only once that task has finished.
 */
#include "nimconfig.h"
#include "esp_bt.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nimble/nimble/host/include/host/ble_hs.h"
#include "nimble/nimble/include/nimble/nimble_npl.h"
#include "nimble/esp_port/esp-hci/include/esp_nimble_hci.h"
#include "nimble/porting/nimble/include/nimble/nimble_port.h"

esp_err_t pairble_nimble_port_init(void)
{
    esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
#if defined(CONFIG_IDF_TARGET_ESP32)
    cfg.mode = ESP_BT_MODE_BLE;
    cfg.ble_max_conn = CONFIG_BT_NIMBLE_MAX_CONNECTIONS;
#elif defined(CONFIG_IDF_TARGET_ESP32C3) || defined(CONFIG_IDF_TARGET_ESP32S3)
    cfg.ble_max_act = MYNEWT_VAL(BLE_MAX_CONNECTIONS) + MYNEWT_VAL(BLE_ROLE_BROADCASTER) + MYNEWT_VAL(BLE_ROLE_OBSERVER);
#else
    cfg.nimble_max_connections = CONFIG_BT_NIMBLE_MAX_CONNECTIONS;
#endif
    esp_err_t err = esp_bt_controller_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE("pairble", "Bluetooth controller init failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (err == ESP_OK) {
#if CONFIG_BT_NIMBLE_LEGACY_VHCI_ENABLE
        err = esp_nimble_hci_init();
#endif
    }
    if (err == ESP_OK) {
        err = nimble_port_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE("pairble", "Bluetooth start failed: %s", esp_err_to_name(err));
        esp_bt_controller_disable();
        esp_bt_controller_deinit();
    }
    return err;
}

esp_err_t pairble_nimble_port_deinit(void)
{
    /* The host, the HCI glue (esp_nimble_deinit() calls esp_nimble_hci_deinit()
     * itself) and the controller. */
    return nimble_port_deinit();
}

static SemaphoreHandle_t s_hs_stopped;   /* binary: a second release is harmless */
static SemaphoreHandle_t s_task_done;
static struct ble_hs_stop_listener s_stop_listener;
static struct ble_npl_event s_quit_ev;
static volatile bool s_quit;

static void quit_cb(struct ble_npl_event *ev)
{
    s_quit = true;
}

void pairble_nimble_host_task(void *param)
{
    struct ble_npl_eventq *q = nimble_port_get_dflt_eventq();
    while (!s_quit) {
        struct ble_npl_event *ev = ble_npl_eventq_get(q, BLE_NPL_TIME_FOREVER);
        if (ev == NULL) {
            continue;
        }
#if !CONFIG_BT_LE_CONTROLLER_NPL_OS_PORTING_SUPPORT
        /* NimBLE-Arduino's ble_hs_timer_reset() deinitialises the host timer
         * while the host is stopping - with its expiry event possibly still
         * in this queue. Run that and the task calls NULL. */
        if (ev->fn == NULL) {
            continue;
        }
#endif
        ble_npl_event_run(ev);
    }
    s_quit = false;
    xSemaphoreGive(s_task_done);
    vTaskDelete(NULL);
}

static void hs_stopped_cb(int status, void *arg)
{
    xSemaphoreGive(s_hs_stopped);
}

int pairble_nimble_port_stop(void)
{
    if (s_hs_stopped == NULL) {
        s_hs_stopped = xSemaphoreCreateBinary();
        s_task_done = xSemaphoreCreateBinary();
        if (s_hs_stopped == NULL || s_task_done == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    xSemaphoreTake(s_hs_stopped, 0);
    xSemaphoreTake(s_task_done, 0);

    /* Ends connections and advertising; the callback runs on the host task. */
    int rc = ble_hs_stop(&s_stop_listener, hs_stopped_cb, NULL);
    if (rc == 0) {
        if (xSemaphoreTake(s_hs_stopped, pdMS_TO_TICKS(5000)) != pdTRUE) {
            /* Connections may still be open: tearing the host down now would
             * pull it from under them. Stay up; protocomm reports the error. */
            ESP_LOGE("pairble", "Bluetooth host did not stop");
            return BLE_HS_ETIMEOUT;
        }
    } else if (rc != BLE_HS_EALREADY) {
        return rc;
    }

    /* Then let the host task leave its loop, and wait until it has. */
    ble_npl_event_init(&s_quit_ev, quit_cb, NULL);
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_quit_ev);
    if (xSemaphoreTake(s_task_done, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE("pairble", "Bluetooth host task did not stop");
        return ESP_ERR_TIMEOUT;
    }
    ble_npl_event_deinit(&s_quit_ev);
    return 0;
}
