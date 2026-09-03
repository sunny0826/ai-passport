idf_component_register(
    SRCS "main.c"
         "ui_pixel.c"
         "ui_pixel_math.c"
         "demo_display.c"
         "demo_button.c"
         "demo_audio.c"
         "demo_battery.c"
         "demo_radio.c"
         "demo_wifi.c"
         "demo_ble.c"
         "demo_low_power.c"
         "demo_pokedex.c"
         "screenshot.c"
         "pokedex_layout.c"
         "pokedex_static.c"
         "pokedex_core.c"
         "pokedex_sprite.c"
         "pokedex_cry.c"
         "pokedex_cry_play.c"
         "vendor/lodepng.c"
    INCLUDE_DIRS "."
    REQUIRES bsp bt esp_audio_codec esp_driver_usb_serial_jtag esp_event esp_hw_support
             esp_netif esp_partition esp_timer esp_wifi nvs_flash
)

# 离线精灵图 blob:嵌入只读段,符号 _binary_pokedex_sprites_bin_start/_end。
target_add_binary_data(${COMPONENT_LIB} "pokedex_sprites.bin" TEXT)