idf_component_register(SRCS
                           "main.c"
                           "audio/audio_i2s.c"
                           "audio/audio_arbiter.c"
                           "dsp/dsp_core.c"
                           "dsp/dsp_biquad.c"
                           "dsp/dsp_harmonics.c"
                           "dsp/dsp_mixer_fir.c"
                           "dsp/dsp_nvs.c"
                           "usb/usb_uac.c"
                           "usb/usb_hid.c"
                           "ui/ui_ssd1306.c"
                           "ui/ui_oled.c"
                           "hal/hal_controls.c"
                           "hal/hal_led.c"
                       INCLUDE_DIRS "." "audio" "dsp" "usb" "ui" "hal"
                       PRIV_REQUIRES esp_driver_i2s esp_driver_gpio esp_driver_pcnt esp_driver_i2c esp_driver_rmt esp_pm esp_timer nvs_flash freertos log espressif__esp_tinyusb espressif__tinyusb espressif__esp-dsp)

