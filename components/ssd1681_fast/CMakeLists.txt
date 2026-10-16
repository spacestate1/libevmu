idf_component_register(SRCS "ssd1681_fast.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_gpio esp_driver_spi esp_timer)
