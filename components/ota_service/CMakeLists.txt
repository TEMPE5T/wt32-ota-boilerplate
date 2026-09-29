idf_component_register(
    SRCS "ota_service.c"
    INCLUDE_DIRS "include"
    REQUIRES app_update bootloader_support esp_app_format esp_http_server esp_system
             wt32_eth user_app log freertos
)
