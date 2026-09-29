#include "wt32_eth.h"

#include <stdio.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_eth.h"
#include "esp_eth_mac_esp.h"
#include "esp_eth_phy.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "wt32_eth";

/* WT32-ETH01 v1.2+ : LAN8720, PHY address 1, oscillator enable on GPIO16. */
#define WT32_PHY_ADDR 1
#define WT32_PHY_POWER_GPIO GPIO_NUM_16
#define WT32_MDC_GPIO 23
#define WT32_MDIO_GPIO 18
#define WT32_OSC_SETTLE_MS 300

static EventGroupHandle_t s_events;
static esp_netif_t *s_netif;
static volatile bool s_force_static;
static volatile const char *s_addr_mode = "none";
static bool s_static_valid;
static esp_netif_ip_info_t s_static_info;
static esp_ip4_addr_t s_static_dns;

static void apply_static_ip(void)
{
    if (!s_static_valid || s_netif == NULL) {
        ESP_LOGE(TAG, "failsafe static IP is not usable");
        return;
    }

    esp_err_t err = esp_netif_dhcpc_stop(s_netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_LOGW(TAG, "stopping DHCP client: %s", esp_err_to_name(err));
    }

    err = esp_netif_set_ip_info(s_netif, &s_static_info);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_set_ip_info failed: %s", esp_err_to_name(err));
        return;
    }

    esp_netif_dns_info_t dns = {0};
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4 = s_static_dns;
    err = esp_netif_set_dns_info(s_netif, ESP_NETIF_DNS_MAIN, &dns);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "setting DNS: %s", esp_err_to_name(err));
    }

    s_addr_mode = "static";
    xEventGroupSetBits(s_events, WT32_IP_READY_BIT);
    ESP_LOGW(TAG,
             "failsafe static address " IPSTR " mask " IPSTR " gateway " IPSTR,
             IP2STR(&s_static_info.ip),
             IP2STR(&s_static_info.netmask),
             IP2STR(&s_static_info.gw));
}

static void on_eth_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;

    if (id == ETHERNET_EVENT_CONNECTED) {
        xEventGroupSetBits(s_events, WT32_LINK_UP_BIT);
        ESP_LOGI(TAG, "link up");
        /*
         * The default netif glue starts the DHCP client on this event.
         * Re-apply the static address afterwards when that is the selected mode.
         */
        if (s_force_static) {
            apply_static_ip();
        }
    } else if (id == ETHERNET_EVENT_DISCONNECTED) {
        xEventGroupClearBits(s_events, WT32_LINK_UP_BIT);
        ESP_LOGW(TAG, "link down");
        if (!s_force_static) {
            s_addr_mode = "none";
            xEventGroupClearBits(s_events, WT32_IP_READY_BIT);
        }
    } else if (id == ETHERNET_EVENT_START) {
        ESP_LOGI(TAG, "EMAC started");
    } else if (id == ETHERNET_EVENT_STOP) {
        ESP_LOGW(TAG, "EMAC stopped");
    }
}

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;

    if (s_force_static) {
        ESP_LOGI(TAG, "ignoring a DHCP lease because the static failsafe is active");
        return;
    }

    const ip_event_got_ip_t *event = data;
    s_addr_mode = "dhcp";
    xEventGroupSetBits(s_events, WT32_IP_READY_BIT);
    ESP_LOGI(TAG, "DHCP lease " IPSTR, IP2STR(&event->ip_info.ip));
}

static bool parse_ip(const char *text, esp_ip4_addr_t *out, const char *label)
{
    if (esp_netif_str_to_ip4(text, out) != ESP_OK) {
        ESP_LOGE(TAG,
                 "invalid %s \"%s\" — set it under menuconfig → WT32 Boilerplate before flashing",
                 label,
                 text);
        return false;
    }
    return true;
}

static void load_static_config(void)
{
    s_static_valid = parse_ip(CONFIG_WT32_STATIC_IP, &s_static_info.ip, "static IP") &&
                     parse_ip(CONFIG_WT32_STATIC_NETMASK, &s_static_info.netmask, "netmask") &&
                     parse_ip(CONFIG_WT32_STATIC_GATEWAY, &s_static_info.gw, "gateway") &&
                     parse_ip(CONFIG_WT32_STATIC_DNS, &s_static_dns, "DNS");

    if (s_static_valid) {
#if CONFIG_WT32_USE_DHCP
        ESP_LOGI(TAG, "compiled failsafe address " IPSTR " (DHCP first)", IP2STR(&s_static_info.ip));
#else
        ESP_LOGI(TAG, "compiled failsafe address " IPSTR " (DHCP disabled)", IP2STR(&s_static_info.ip));
#endif
    }
}

static void enable_phy_oscillator(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << WT32_PHY_POWER_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    ESP_ERROR_CHECK(gpio_set_level(WT32_PHY_POWER_GPIO, 1));
    /*
     * GPIO0 is the RMII clock input and a strapping pin. The board keeps the
     * oscillator off until GPIO16 is high so GPIO0 is idle while the ROM
     * samples the strap. Give the 50 MHz clock time to run before the MAC starts.
     */
    vTaskDelay(pdMS_TO_TICKS(WT32_OSC_SETTLE_MS));
    ESP_LOGI(TAG, "PHY oscillator enabled on GPIO%d", (int)WT32_PHY_POWER_GPIO);
}

esp_err_t wt32_eth_start(void)
{
    s_events = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(s_events != NULL, ESP_ERR_NO_MEM, TAG, "event group");

    load_static_config();
    enable_phy_oscillator();

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_esp32_emac_config_t emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac_config.smi_gpio.mdc_num = WT32_MDC_GPIO;
    emac_config.smi_gpio.mdio_num = WT32_MDIO_GPIO;
    emac_config.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;
    /* GPIO0 is the only ESP32 pin that can input the RMII clock. 0, not the
     * 5.4-only EMAC_CLK_IN_GPIO name, so this also builds on ESP-IDF 6.x. */
    emac_config.clock_config.rmii.clock_gpio = 0;

    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = WT32_PHY_ADDR;
    phy_config.reset_gpio_num = -1;

    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_config, &mac_config);
    esp_eth_phy_t *phy = esp_eth_phy_new_lan87xx(&phy_config);
    ESP_RETURN_ON_FALSE(mac != NULL && phy != NULL, ESP_FAIL, TAG, "EMAC or PHY alloc failed");

    esp_eth_handle_t eth_handle = NULL;
    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    ESP_RETURN_ON_ERROR(esp_eth_driver_install(&eth_config, &eth_handle), TAG, "driver install");

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    s_netif = esp_netif_new(&netif_cfg);
    ESP_RETURN_ON_FALSE(s_netif != NULL, ESP_FAIL, TAG, "netif alloc failed");
    ESP_RETURN_ON_ERROR(esp_netif_set_hostname(s_netif, "wt32-ota"), TAG, "hostname");

    esp_eth_netif_glue_handle_t glue = esp_eth_new_netif_glue(eth_handle);
    ESP_RETURN_ON_FALSE(glue != NULL, ESP_FAIL, TAG, "netif glue alloc failed");
    ESP_RETURN_ON_ERROR(esp_netif_attach(s_netif, glue), TAG, "netif attach");

    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, on_eth_event, NULL),
        TAG,
        "eth handler");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, on_got_ip, NULL),
        TAG,
        "ip handler");

#if CONFIG_WT32_USE_DHCP
    s_force_static = false;
#else
    s_force_static = true;
#endif

    ESP_RETURN_ON_ERROR(esp_eth_start(eth_handle), TAG, "eth start");
    xEventGroupSetBits(s_events, WT32_ETH_READY_BIT);

    if (s_force_static) {
        if (!s_static_valid) {
            ESP_LOGE(TAG, "DHCP is disabled and the static address is invalid; no IP installed");
            return ESP_OK;
        }
        apply_static_ip();
        return ESP_OK;
    }

    ESP_LOGI(TAG, "waiting up to %d s for a DHCP lease", CONFIG_WT32_DHCP_TIMEOUT_S);
    EventBits_t bits = xEventGroupWaitBits(s_events,
                                            WT32_IP_READY_BIT,
                                            pdFALSE,
                                            pdTRUE,
                                            pdMS_TO_TICKS(CONFIG_WT32_DHCP_TIMEOUT_S * 1000));
    if (bits & WT32_IP_READY_BIT) {
        ESP_LOGI(TAG, "DHCP succeeded");
        return ESP_OK;
    }

    ESP_LOGW(TAG, "DHCP timed out after %d s", CONFIG_WT32_DHCP_TIMEOUT_S);
    s_force_static = true;
    if (!s_static_valid) {
        ESP_LOGE(TAG, "static failsafe is invalid; the board has no IP address");
        return ESP_OK;
    }
    apply_static_ip();
    return ESP_OK;
}

EventGroupHandle_t wt32_eth_events(void)
{
    return s_events;
}

bool wt32_eth_driver_ready(void)
{
    if (s_events == NULL) {
        return false;
    }
    return (xEventGroupGetBits(s_events) & WT32_ETH_READY_BIT) != 0;
}

bool wt32_eth_link_is_up(void)
{
    if (s_events == NULL) {
        return false;
    }
    return (xEventGroupGetBits(s_events) & WT32_LINK_UP_BIT) != 0;
}

bool wt32_eth_has_ip(void)
{
    if (s_events == NULL) {
        return false;
    }
    return (xEventGroupGetBits(s_events) & WT32_IP_READY_BIT) != 0;
}

const char *wt32_eth_addr_mode(void)
{
    return (const char *)s_addr_mode;
}

esp_err_t wt32_eth_get_ip_str(char *buf, size_t len)
{
    if (buf == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_netif == NULL) {
        snprintf(buf, len, "0.0.0.0");
        return ESP_ERR_INVALID_STATE;
    }

    esp_netif_ip_info_t info = {0};
    esp_err_t err = esp_netif_get_ip_info(s_netif, &info);
    if (err != ESP_OK) {
        snprintf(buf, len, "0.0.0.0");
        return err;
    }
    esp_ip4addr_ntoa(&info.ip, buf, len);
    return ESP_OK;
}
