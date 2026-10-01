/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Where the proof of possession lives: written at manufacturing time by
 * @pairble/cli so it matches a printed label, or generated on first boot for
 * devices that show their QR code themselves.
 */

#include "nvs.h"
#include "pairble_internal.h"

esp_err_t pairble_nvs_get_pop(char *out, size_t len)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("pairble", NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }
    size_t required = len;
    err = nvs_get_str(handle, "pop", out, &required);
    nvs_close(handle);
    return err;
}

esp_err_t pairble_nvs_set_pop(const char *pop)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("pairble", NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(handle, "pop", pop);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t pairble_nvs_get_blob(const char *key, void *out, size_t len)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("pairble", NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }
    size_t actual = len;
    err = nvs_get_blob(handle, key, out, &actual);
    nvs_close(handle);
    if (err == ESP_OK && actual != len) {
        err = ESP_ERR_NVS_INVALID_LENGTH;
    }
    return err;
}

esp_err_t pairble_nvs_set_blob(const char *key, const void *data, size_t len)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("pairble", NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(handle, key, data, len);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t pairble_nvs_erase(const char *key)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("pairble", NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_key(handle, key);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
}
