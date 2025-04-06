idf_component_register(SRCS "hello_world_main.c" "led_strip_encoder.c"
                    REQUIRES esp_driver_rmt esp_driver_gpio esp_lcd
                    PRIV_REQUIRES spi_flash
                    INCLUDE_DIRS ".")
