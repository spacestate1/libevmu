idf_component_register(SRCS "main.c" "drive_counters.c" "link.c"
                       INCLUDE_DIRS "."
                       REQUIRES evmu ssd1681_fast fatfs esp_timer esp_driver_gpio esp_driver_i2c esp_driver_usb_serial_jtag esp_rom)

# Wear-levelled FAT image with BIOS.BIN and the VMU flash image VMU.BIN.
# Note: "idf.py flash" rewrites it, resetting VMU.BIN to flash_data/VMU.BIN.
fatfs_create_spiflash_image(storage ${CMAKE_CURRENT_SOURCE_DIR}/../flash_data FLASH_IN_PROJECT)
