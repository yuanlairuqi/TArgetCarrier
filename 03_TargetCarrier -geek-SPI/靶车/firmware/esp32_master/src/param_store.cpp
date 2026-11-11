/**
 * @file    param_store.cpp
 * @brief   靶车 —— 主机参数掉电保存 (直接用 IDF 的 NVS, 不额外依赖 Preferences 库)
 *
 *  存储: namespace "targetcar", key "params", 一个二进制 blob + 魔数校验。
 *  ★ 用 blob 而不是逐个 key: 一次读写、结构体一眼看懂, 字段变了改魔数即可。
 */

#include "param_store.h"

#include <string.h>

#include <nvs.h>
#include <nvs_flash.h>

#define NVS_NAMESPACE   "targetcar"
#define NVS_KEY         "params"

static bool s_nvs_ready = false;

static bool ensure_nvs(void)
{
    if (s_nvs_ready) {
        return true;
    }
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* 分区被写过别的布局 / 版本不兼容: 擦掉重建 */
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        return false;
    }
    s_nvs_ready = true;
    return true;
}

bool param_store_load(param_blob_t *out)
{
    if (out == NULL || !ensure_nvs()) {
        return false;
    }

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;   /* 还没存过 */
    }

    param_blob_t tmp;
    size_t len = sizeof(tmp);
    esp_err_t err = nvs_get_blob(h, NVS_KEY, &tmp, &len);
    nvs_close(h);

    if (err != ESP_OK || len != sizeof(tmp) || tmp.magic != PARAM_STORE_MAGIC) {
        return false;
    }

    *out = tmp;
    return true;
}

bool param_store_save(const param_blob_t *in)
{
    if (in == NULL || !ensure_nvs()) {
        return false;
    }

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }

    param_blob_t tmp = *in;
    tmp.magic = PARAM_STORE_MAGIC;

    esp_err_t err = nvs_set_blob(h, NVS_KEY, &tmp, sizeof(tmp));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err == ESP_OK;
}

/* ================================================================== */
/* 参数预设槽位                                                       */
/* ================================================================== */

#define PRESET_MAGIC   0x50524553u   /* "PRES" */

static void preset_key(uint8_t slot, char out[8])
{
    /* "pre1" .. "pre4" */
    out[0] = 'p'; out[1] = 'r'; out[2] = 'e';
    out[3] = (char)('1' + slot);
    out[4] = '\0';
}

bool preset_save(uint8_t slot, const char *name, const param_blob_t *params)
{
    if (slot >= PRESET_SLOTS || params == NULL || !ensure_nvs()) {
        return false;
    }

    preset_blob_t p;
    memset(&p, 0, sizeof(p));
    p.magic = PRESET_MAGIC;
    p.params = *params;
    p.params.magic = PARAM_STORE_MAGIC;

    /* 名字直接进 JSON 输出, 所以先过滤成安全字符:
     * 引号/反斜杠/控制字符都会破坏前端 JSON.parse —— 统一换成 '_'。 */
    if (name != NULL) {
        size_t k = 0;
        for (const char *s = name; *s != '\0' && k < PRESET_NAME_LEN - 1; s++) {
            unsigned char c = (unsigned char)*s;
            bool safe = (c >= 0x20u && c != '"' && c != '\\' && c != 0x7Fu);
            p.name[k++] = safe ? (char)c : '_';
        }
        p.name[k] = '\0';
    }

    char key[8];
    preset_key(slot, key);

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    esp_err_t err = nvs_set_blob(h, key, &p, sizeof(p));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err == ESP_OK;
}

bool preset_load(uint8_t slot, preset_blob_t *out)
{
    if (slot >= PRESET_SLOTS || out == NULL || !ensure_nvs()) {
        return false;
    }

    char key[8];
    preset_key(slot, key);

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t len = sizeof(*out);
    esp_err_t err = nvs_get_blob(h, key, out, &len);
    nvs_close(h);

    if (err != ESP_OK || len != sizeof(*out) || out->magic != PRESET_MAGIC) {
        return false;
    }
    return true;
}

bool preset_clear(uint8_t slot)
{
    if (slot >= PRESET_SLOTS || !ensure_nvs()) {
        return false;
    }

    char key[8];
    preset_key(slot, key);

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    nvs_erase_key(h, key);
    nvs_commit(h);
    nvs_close(h);
    return true;
}
