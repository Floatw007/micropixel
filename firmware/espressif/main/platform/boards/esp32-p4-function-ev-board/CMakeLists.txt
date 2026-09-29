list(APPEND micropixel_sources
    "platform/boards/esp32-p4-function-ev-board/board_hardware.cpp"
    "platform/boards/esp32-p4-function-ev-board/display_idle_controller.cpp"
    "platform/boards/esp32-p4-function-ev-board/display/display_pipeline.cpp"
    "platform/boards/esp32-p4-function-ev-board/idle_frequency_telemetry.cpp"
    "platform/boards/esp32-p4-function-ev-board/i2s_audio.cpp"
    "platform/boards/esp32-p4-function-ev-board/platform.cpp"
    "platform/boards/esp32-p4-function-ev-board/presentation.cpp"
    "platform/input/esp_lcd_touch_input.cpp"
    "platform/storage/sd_card_block_storage.cpp"
    "platform/transports/development_display_control.cpp"
    "platform/transports/usb_serial_jtag_local_control.cpp"
    "platform/wifi/esp_hosted_radio.cpp"
    "platform/wifi/wifi_manager.cpp"
)

# The profile builds an ESP32-C6 ESP-Hosted companion image first. Register it
# with ESP-IDF's normal flash manifest so `flash` and `flash-built` stage the
# exact image consumed by EspHostedRadio's one-time SDIO OTA path.
set(micropixel_c6_slave_image
    "${CMAKE_CURRENT_LIST_DIR}/../../../../../../build/esp-hosted-c6-slave/network_adapter.bin")
if(EXISTS "${micropixel_c6_slave_image}")
    esptool_py_flash_to_partition(flash slave_fw "${micropixel_c6_slave_image}")
else()
    message(FATAL_ERROR
        "ESP32-C6 companion image is missing: ${micropixel_c6_slave_image}. "
        "Build the esp32-p4-function-ev profile through tools/firmware.py.")
endif()
