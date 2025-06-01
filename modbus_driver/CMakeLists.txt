idf_component_register(SRCS "modbus_driver.c"
                    INCLUDE_DIRS "include"
                    REQUIRES driver esp_timer lock_controller_driver)