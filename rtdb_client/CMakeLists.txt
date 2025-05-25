idf_component_register(SRCS "rtdb_client.c"
                       INCLUDE_DIRS "include" "../../include"
                       REQUIRES esp_http_client nvs_flash esp_wifi esp_event esp_netif mbedtls json driver esp_timer
                      )