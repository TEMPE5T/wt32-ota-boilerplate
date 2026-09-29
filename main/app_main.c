#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "ota_service.h"
#include "user_app.h"
#include "wt32_eth.h"

static const char *TAG = "app_main";

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition was truncated or has a new version; erasing");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(wt32_eth_start());
    ESP_ERROR_CHECK(ota_service_start());

    /*
     * Confirm or roll back before user tasks run. A new image that crashes
     * inside this call, or resets before it returns, is discarded by the
     * bootloader. Checks that must pass before the image is trusted belong
     * in user_app_diagnostics().
     */
    ESP_ERROR_CHECK(ota_service_confirm_image());

    user_app_start();
    ESP_LOGI(TAG, "boilerplate is up; application tasks were started");
}
