idf_component_register(
    SRCS "wifi_manager.cpp"
    INCLUDE_DIRS "."
    REQUIRES wifi_provisioning esp_wifi esp_event esp_netif log esp_common nvs_flash)

set(COMPONENT_KCONFIG Kconfig)