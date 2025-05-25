idf_component_register(
                        SRCS "wifi_manager.c"
                        INCLUDE_DIRS "include" "../../include"
                        REQUIRES wifi_provisioning esp_event driver esp_timer nvs_flash esp_netif esp_http_server
)