idf_component_register(
    SRCS
        # Boot and real-time audio / device coordination.
        "main.c"
        "app_audio.c"
        "app_acoustic.c"
        "app_agent.c"
        "app_desktop.c"
        "app_control.c"
        "app_media.c"
        "app_media_web.c"
        "app_focus.c"
        "app_words.c"
        "app_games.c"
        "app_games_core.c"
        "app_wake.c"
        "app_wake_diagnostic.c"
        "app_barge_diagnostic.c"
        "app_devices.c"
        "app_sse.c"
        "app_button.c"
        # Cloud protocol, bounded tool loop, context and event history.
        "app_cloud.c"
        "app_cloud_http.c"
        "app_cloud_asr.c"
        "app_cloud_tts.c"
        "app_conversation.c"
        "app_history_store.c"
        "app_turn.c"
        "app_turn_log.c"
        # Board services and LVGL views; font sources are generated assets.
        "app_display.c"
        "app_network.c"
        "app_motion.c"
        "app_weather.c"
        "app_storage.c"
        "app_ui.c"
        "app_ui_font_14.c"
        "app_ui_font_16.c"
        "app_ui_font_20.c"
        "app_ui_font_digits_56.c"
        "app_ui_font_weather_24.c"
    INCLUDE_DIRS "."
    LDFRAGMENTS "lvgl_psram.lf"
    EMBED_FILES "reminder.pcm"
    EMBED_TXTFILES "media_web.html"
    PRIV_REQUIRES
        esp_lcd
        esp_driver_spi
        esp_driver_i2s
        esp_driver_i2c
        esp_driver_ledc
        esp_wifi
        esp_event
        esp_netif
        esp_http_client
        esp_http_server
        esp_websocket_client
        espressif__esp-sr
        esp_timer
        esp_hw_support
        mbedtls
        heap
        nvs_flash
        fatfs
)
