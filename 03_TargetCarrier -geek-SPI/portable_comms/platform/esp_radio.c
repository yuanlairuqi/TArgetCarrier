#include "tc_radio.h"
#include <string.h>
#include <stdatomic.h>
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "psa/crypto.h"
#if __has_include("tc_pairing.local.h")
#include "tc_pairing.local.h"
#else
#include "tc_pairing.example.h"
#endif
#define MAC_ALG PSA_ALG_TRUNCATED_MAC(PSA_ALG_HMAC(PSA_ALG_SHA_256), 16)
typedef struct {
    uint8_t b[144];
    uint32_t at;
} packet;
static QueueHandle_t rxhi, rxlo;
static tc_queue txhi, txlo;
static psa_key_id_t key;
static uint8_t peer[6] = TC_PEER_MAC;
static uint8_t pmk[16] = TC_PMK, lmk[16] = TC_LMK, auth[32] = TC_AUTH_KEY;
static uint8_t local[6], sending[144];
static uint32_t started, last_wave;
static atomic_bool usable, busy;
static atomic_uint ok, fail, drops, authfail;
static uint32_t timeout, expired;
static bool configured;
static void on_send(const esp_now_send_info_t *i, esp_now_send_status_t s) {
    (void)i;
    if (s == ESP_NOW_SEND_SUCCESS)
        atomic_fetch_add(&ok, 1);
    else
        atomic_fetch_add(&fail, 1);
    atomic_store(&busy, false);
}
static void on_recv(const esp_now_recv_info_t *i, const uint8_t *d, int n) {
    if (!atomic_load(&usable) || !i || !d || n != 144 || memcmp(i->src_addr, peer, 6) ||
        memcmp(i->des_addr, local, 6))
        return;
    packet p = {.at = (uint32_t)(esp_timer_get_time() / 1000)};
    memcpy(p.b, d, 144);
    if (xQueueSend(d[3] == TC_WAVE ? rxlo : rxhi, &p, 0) != pdTRUE)
        atomic_fetch_add(&drops, 1);
}
static bool nonzero(const uint8_t *b, size_t n) {
    uint8_t v = 0;
    for (size_t i = 0; i < n; i++)
        v |= b[i];
    return v != 0;
}
bool tc_radio_init(void) {
    if (!TC_RADIO_ENABLED)
        return false;
    if ((peer[0] & 1) || !nonzero(peer, 6) || !nonzero(pmk, 16) || !nonzero(lmk, 16) ||
        !nonzero(auth, 32) || TC_WIFI_CHANNEL < 1 || TC_WIFI_CHANNEL > 13)
        return false;
    if (psa_crypto_init() != PSA_SUCCESS)
        return false;
    psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&a, PSA_KEY_TYPE_HMAC);
    psa_set_key_bits(&a, 256);
    psa_set_key_algorithm(&a, MAC_ALG);
    psa_set_key_usage_flags(&a, PSA_KEY_USAGE_SIGN_MESSAGE | PSA_KEY_USAGE_VERIFY_MESSAGE);
    psa_status_t s = psa_import_key(&a, auth, 32, &key);
    psa_reset_key_attributes(&a);
    memset(auth, 0, 32);
    if (s != PSA_SUCCESS)
        return false;
    rxhi = xQueueCreate(8, sizeof(packet));
    rxlo = xQueueCreate(4, sizeof(packet));
    if (!rxhi || !rxlo)
        return false;
    if (esp_netif_init() != ESP_OK || esp_event_loop_create_default() != ESP_OK)
        return false;
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&cfg) != ESP_OK || esp_wifi_set_storage(WIFI_STORAGE_RAM) != ESP_OK ||
        esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK || esp_wifi_start() != ESP_OK ||
        esp_wifi_set_ps(WIFI_PS_NONE) != ESP_OK ||
        esp_wifi_set_channel(TC_WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE) != ESP_OK ||
        esp_wifi_get_mac(WIFI_IF_STA, local) != ESP_OK || !memcmp(peer, local, 6))
        return false;
    if (esp_now_init() != ESP_OK || esp_now_set_pmk(pmk) != ESP_OK)
        return false;
    esp_now_peer_info_t p = {.channel = TC_WIFI_CHANNEL, .ifidx = WIFI_IF_STA, .encrypt = true};
    memcpy(p.peer_addr, peer, 6);
    memcpy(p.lmk, lmk, 16);
    if (esp_now_add_peer(&p) != ESP_OK || esp_now_register_recv_cb(on_recv) != ESP_OK ||
        esp_now_register_send_cb(on_send) != ESP_OK)
        return false;
    configured = true;
    atomic_store(&usable, true);
    return true;
}
bool tc_radio_send(const uint8_t *b, uint32_t now) {
    if (!atomic_load(&usable))
        return false;
    return tc_push(b[3] == TC_WAVE ? &txlo : &txhi, b, now, b[3] == TC_WAVE);
}
void tc_radio_poll(uint32_t now) {
    if (!atomic_load(&usable))
        return;
    if (atomic_load(&busy)) {
        if (now - started > 250) {
            timeout++; /* Fail closed: late untagged MAC callback cannot be assigned to a new
                          submission. */
            atomic_store(&usable, false);
        }
        return;
    }
    uint8_t b[128];
    bool got = tc_pop(&txhi, b, now, 100);
    if (!got && now - last_wave >= 20) {
        got = tc_pop(&txlo, b, now, 100);
        if (got)
            last_wave = now;
    }
    if (!got)
        return;
    memcpy(sending, b, 128);
    size_t size = 0;
    if (psa_mac_compute(key, MAC_ALG, b, 128, sending + 128, 16, &size) != PSA_SUCCESS ||
        size != 16) {
        authfail++;
        return;
    }
    started = now;
    atomic_store(&busy, true);
    if (esp_now_send(peer, sending, 144) != ESP_OK) {
        atomic_store(&busy, false);
        atomic_fetch_add(&fail, 1);
    }
}
bool tc_radio_receive(uint8_t *b, uint32_t now) {
    if (!configured)
        return false;
    for (int k = 0; k < 8; k++) {
        packet p;
        if (xQueueReceive(rxhi, &p, 0) != pdTRUE && xQueueReceive(rxlo, &p, 0) != pdTRUE)
            return false;
        if (now - p.at > 100) {
            expired++;
            continue;
        }
        if (psa_mac_verify(key, MAC_ALG, p.b, 128, p.b + 128, 16) != PSA_SUCCESS) {
            atomic_fetch_add(&authfail, 1);
            continue;
        }
        tc_frame f;
        if (!tc_decode(p.b, &f))
            continue;
        memcpy(b, p.b, 128);
        return true;
    }
    return false;
}
void tc_radio_get_stats(tc_radio_stats *s) {
    *s = (tc_radio_stats){
        atomic_load(&ok), atomic_load(&fail), atomic_load(&drops), atomic_load(&authfail), timeout,
        expired,          configured,         atomic_load(&usable)};
}
