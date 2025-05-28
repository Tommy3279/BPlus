idf_component_register(SRCS "main_utils.c"
                    INCLUDE_DIRS "include" "../../include"  # Đường dẫn include cho main_utils
                    REQUIRES
                        esp_ghota    
                        keypad16        # Component keypad
                        display       # Component display
                        wifi_manager    # Component wifi_manager
                        rtdb_client
                        nvs_flash
                        freertos
                        esp_event
                        esp_wifi
                        lock_controller_driver
                    )
