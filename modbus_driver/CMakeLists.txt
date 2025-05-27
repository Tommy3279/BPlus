idf_component_register(SRCS "modbus_driver.c"
                    INCLUDE_DIRS "include" "../../include"
                    REQUIRES driver esp_timer)