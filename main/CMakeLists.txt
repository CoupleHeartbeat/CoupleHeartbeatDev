idf_component_register(
    SRCS "main.c"
         "couple_audio.c"
         "couple_mode_manager.c"
         "couple_badge.c"
         "couple_ble.c"
         "couple_auto.c"
         "couple_crypto.c"
         "couple_storage.c"
         "couple_gatt.c"
         "couple_time.c"
         "couple_power.c"
         "couple_power_exec.c"
         "couple_userfs.c"
         "couple_ai.c"
         "couple_voice.c"
         "couple_fonts.c"
         "demo_radio.c"
         "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/couple_font_16.c"
         "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/couple_font_20.c"
    INCLUDE_DIRS "."
    REQUIRES bsp bt esp_event esp_hw_support esp_timer fatfs wear_levelling
             nvs_flash mbedtls json
)
