idf_component_register(
    SRCS "net_report.c"
    INCLUDE_DIRS "include"
    REQUIRES app_config esp_wifi esp_http_client esp_event esp_netif esp_timer
)
