#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define OLED_WIDTH       128
#define OLED_HEIGHT      64
#define OLED_BUFFER_SIZE (OLED_WIDTH * OLED_HEIGHT / 8)

esp_err_t ssd1306_bus_init(void);
esp_err_t ssd1306_probe_and_init(void);
bool      ssd1306_is_online(void);
esp_err_t ssd1306_send_cmd1(uint8_t c1);
esp_err_t ssd1306_send_cmd2(uint8_t c1, uint8_t c2);
void      ssd1306_set_pixel_shift(int shift_x, int shift_y);

void      oled_clear(void);
void      oled_draw_pixel(int x, int y, bool on);
void      oled_draw_char(int x, int y, char c, bool invert);
void      oled_draw_string(int x, int y, const char *str, bool invert);
void      oled_draw_hline(int x, int y, int w, bool on);
void      oled_draw_vline(int x, int y, int h, bool on);
void      oled_draw_rect(int x, int y, int w, int h, bool fill, bool on);
void      oled_draw_progress_bar(int x, int y, int w, int h, int percent);
esp_err_t oled_flush(void);
