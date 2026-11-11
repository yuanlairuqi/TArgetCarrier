/**
 * @file    netcfg.cpp
 * @brief   靶车 —— WiFi 账号的 NVS 存取 (IDF NVS, 与 param_store 同一套)
 */

#include "netcfg.h"

#include <string.h>
#include <nvs.h>
#include <nvs_flash.h>

#define NVS_NAMESPACE   "targetcar"
#define KEY_SSID        "wifi_ssid"
#define KEY_PASS        "wifi_pass"

static bool s_nvs_ready = false;

static bool ensure_nvs(void)
{
    if (s_nvs_ready) {
        return true;
    }
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        return false;
    }
    s_nvs_ready = true;
    return true;
}

bool netcfg_load(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    if (ssid == NULL || pass == NULL || !ensure_nvs()) {
        return false;
    }
    ssid[0] = '\0';
    pass[0] = '\0';

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }

    size_t n = ssid_len;
    esp_err_t e1 = nvs_get_str(h, KEY_SSID, ssid, &n);
    n = pass_len;
    esp_err_t e2 = nvs_get_str(h, KEY_PASS, pass, &n);
    nvs_close(h);

    return (e1 == ESP_OK && e2 == ESP_OK && ssid[0] != '\0');
}

bool netcfg_save(const char *ssid, const char *pass)
{
    if (ssid == NULL || pass == NULL || ssid[0] == '\0' || !ensure_nvs()) {
        return false;
    }

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    esp_err_t err = nvs_set_str(h, KEY_SSID, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(h, KEY_PASS, pass);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err == ESP_OK;
}

void netcfg_clear(void)
{
    if (!ensure_nvs()) {
        return;
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_erase_key(h, KEY_SSID);
    nvs_erase_key(h, KEY_PASS);
    nvs_commit(h);
    nvs_close(h);
}
