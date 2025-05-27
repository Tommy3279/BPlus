idf_component_register(SRCS "lock_controller_driver.c"
                    INCLUDE_DIRS "include" "../../include"
                    REQUIRES driver esp_timer modbus_driver)
