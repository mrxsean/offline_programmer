#include "mock_idf.h"

#define CONFIG_PROG_WIFI_SSID "RL78-Flasher"
#define CONFIG_PROG_WIFI_PASSWORD "12345678"
#define CONFIG_PROG_WIFI_CHANNEL 1
#define WIFI_EVENT ((void *)1)
#define WIFI_EVENT_AP_STACONNECTED 1
#define WIFI_EVENT_AP_STADISCONNECTED 2
#define ESP_EVENT_ANY_ID -1
#define ESP_MAC_WIFI_SOFTAP 1
#define WIFI_AUTH_WPA2_PSK 2
#define WIFI_AUTH_OPEN 0
#define WIFI_MODE_AP 1
#define WIFI_IF_AP 1
#define WIFI_PS_NONE 0
typedef void *esp_event_base_t;
typedef struct { uint8_t mac[6]; int aid; } wifi_event_ap_staconnected_t;
typedef wifi_event_ap_staconnected_t wifi_event_ap_stadisconnected_t;
typedef int wifi_init_config_t;
#define WIFI_INIT_CONFIG_DEFAULT() 0
typedef struct {
    struct {
        uint8_t ssid[32], password[64];
        unsigned ssid_len, channel, max_connection, beacon_interval, authmode;
    } ap;
} wifi_config_t;
typedef int esp_netif_t;
typedef struct { int ip; } esp_netif_ip_info_t;
static wifi_config_t captured;
static uint8_t mac_tail[2] = {0x0a, 0x03};
static esp_err_t mac_error;
static bool started;
static esp_err_t esp_netif_init(void) { return ESP_OK; }
static esp_err_t esp_event_loop_create_default(void) { return ESP_OK; }
static void esp_netif_create_default_wifi_ap(void) { }
static esp_err_t esp_wifi_init(wifi_init_config_t *cfg) { return ESP_OK; }
static esp_err_t esp_event_handler_instance_register(esp_event_base_t base, int id,
    void (*cb)(void *, esp_event_base_t, int32_t, void *), void *arg, void *instance) { return ESP_OK; }
static esp_err_t esp_read_mac(uint8_t *mac, int type) {
    assert(type == ESP_MAC_WIFI_SOFTAP);
    memset(mac, 0, 6); mac[4] = mac_tail[0]; mac[5] = mac_tail[1]; return mac_error;
}
static esp_err_t esp_wifi_set_mode(int mode) { assert(mode == WIFI_MODE_AP); return ESP_OK; }
static esp_err_t esp_wifi_set_config(int iface, const wifi_config_t *cfg) {
    assert(iface == WIFI_IF_AP); captured = *cfg; return ESP_OK;
}
static esp_err_t esp_wifi_start(void) { started = true; return ESP_OK; }
static esp_err_t esp_wifi_set_max_tx_power(int power) { return ESP_OK; }
static esp_err_t esp_wifi_set_ps(int ps) { return ESP_OK; }
static esp_netif_t *esp_netif_get_handle_from_ifkey(const char *key) { return NULL; }
static esp_err_t esp_netif_get_ip_info(esp_netif_t *netif, esp_netif_ip_info_t *ip) { return ESP_OK; }

#include "../main/wifi_ap.c"

int main(void) {
    assert(wifi_ap_init() == ESP_OK && started);
    assert(strcmp(wifi_ap_ssid(), "RL78-Flasher-0A03") == 0);
    assert(captured.ap.ssid_len == strlen(wifi_ap_ssid()));
    assert(memcmp(captured.ap.ssid, wifi_ap_ssid(), captured.ap.ssid_len) == 0);
    assert(strcmp((char *)captured.ap.password, CONFIG_PROG_WIFI_PASSWORD) == 0);
    assert(captured.ap.authmode == WIFI_AUTH_WPA2_PSK);
    mac_tail[0] = 0xab; mac_tail[1] = 0xcd;
    assert(wifi_ap_init() == ESP_OK);
    assert(strcmp(wifi_ap_ssid(), "RL78-Flasher-ABCD") == 0);
    mac_error = ESP_FAIL; started = false;
    assert(wifi_ap_init() == ESP_FAIL && !started);
    puts("WiFi: SoftAP MAC suffix, leading zero, shared display name and MAC error PASS");
}
