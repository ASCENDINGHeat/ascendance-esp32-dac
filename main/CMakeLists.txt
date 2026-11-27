idf_component_register(SRCS "main.c" "audio_pipeline.c" "controls.c" "oled_display.c" "dsp/peq_dsp.c" "dsp/peq_biquad.c" "dsp/peq_harmonics.c" "dsp/peq_mixer_fir.c" "dsp/peq_storage_nvs.c" "audio_source_arbiter.c" "usb_uac_audio.c" "webhid_peq.c" "led_indicator.c"
                       INCLUDE_DIRS "." "dsp"
                       PRIV_REQUIRES esp_driver_i2s esp_driver_gpio esp_driver_pcnt esp_driver_i2c esp_driver_rmt esp_pm esp_timer nvs_flash freertos log espressif__esp_tinyusb espressif__tinyusb espressif__esp-dsp)

