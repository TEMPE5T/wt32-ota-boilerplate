#include "user_app.h"

#include "esp_app_desc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wt32_eth.h"

static const char *TAG = "user_app";

static void heartbeat_task(void *arg)
{
    (void)arg;
    const esp_app_desc_t *app = esp_app_get_description();

    for (;;) {
        char ip[16] = "0.0.0.0";
        wt32_eth_get_ip_str(ip, sizeof(ip));
        ESP_LOGI(TAG,
                 "heartbeat %s %s  ip %s (%s)  link %s",
                 app->project_name,
                 app->version,
                 ip,
                 wt32_eth_addr_mode(),
                 wt32_eth_link_is_up() ? "up" : "down");
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

void user_app_start(void)
{
    BaseType_t ok = xTaskCreate(heartbeat_task, "user_hb", 4096, NULL, 5, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "failed to create the heartbeat task");
        return;
    }
    ESP_LOGI(TAG, "heartbeat task started; replace user_app.c with product tasks");
}

bool user_app_diagnostics(void)
{
    return true;
}
