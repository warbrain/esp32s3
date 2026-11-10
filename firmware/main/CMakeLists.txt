idf_component_register(SRCS "main.c" "sleep_digits.c"
                       INCLUDE_DIRS "."
                       REQUIRES nvs_flash esp_wifi esp_event esp_netif esp_timer
                       PRIV_REQUIRES esp_driver_i2s esp_driver_gpio pthread)
