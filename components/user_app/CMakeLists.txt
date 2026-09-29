idf_component_register(
    SRCS "user_app.c"
    INCLUDE_DIRS "include"
    REQUIRES wt32_eth esp_app_format log freertos
)
