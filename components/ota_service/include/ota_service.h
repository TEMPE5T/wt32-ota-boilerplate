#pragma once

#include "esp_err.h"

/**
 * Listen for GET /info and POST /ota on WT32_OTA_PORT.
 * The image is written with esp_ota_begin / esp_ota_write / esp_ota_end
 * into the inactive OTA slot. The boot partition changes only after
 * esp_ota_end accepts the image.
 */
esp_err_t ota_service_start(void);

/**
 * On the first boot of an OTA image (ESP_OTA_IMG_PENDING_VERIFY), run
 * diagnostics and either confirm the image or roll back and reboot.
 * Factory boots and already-confirmed images return immediately.
 */
esp_err_t ota_service_confirm_image(void);
