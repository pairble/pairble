# The GPIO driver became its own component in ESP-IDF 5.3.
if("${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}" VERSION_LESS "5.3")
    set(gpio_driver driver)
else()
    set(gpio_driver esp_driver_gpio)
endif()

idf_component_register(
    SRCS "src/pairble.c"
         "src/pairble_triggers.c"
         "src/pairble_text.c"
         "src/pairble_nvs.c"
         "src/pairble_softap.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    EMBED_FILES "web/setup.html.gz"
    PRIV_REQUIRES bt ${gpio_driver} esp_netif esp_timer protocomm bootloader_support esp_http_server lwip
    REQUIRES esp_wifi esp_event nvs_flash
)
