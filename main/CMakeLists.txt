idf_component_register(SRCS "main.c" "audio_pipeline.c" "controls.c" "oled_display.c" "peq_dsp.c" "audio_source_arbiter.c" "webhid_peq.c" "led_indicator.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_driver_i2s esp_driver_gpio esp_driver_pcnt esp_driver_i2c esp_driver_rmt nvs_flash freertos log espressif__esp_tinyusb espressif__tinyusb)

