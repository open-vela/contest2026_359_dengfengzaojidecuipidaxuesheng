idf_component_register(
    SRCS "main.c" "route_plan.c" "matrix.c" "peripherals.c" "api.c" "net.c" "wifi_ble.c" "secrets.c" "button.c" "script_vm.c" "script_service.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_event esp_netif nvs_flash wifi_provisioning
             esp_driver_gpio esp_driver_i2c esp_driver_spi esp_driver_uart
             esp_driver_ledc esp_adc esp_timer esp_rom mbedtls lwip bootloader_support
)
