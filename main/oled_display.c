#include "oled_display.h"
#include "board_pins.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "driver/i2c_master.h"

#define TAG "OLED"

#define OLED_WIDTH          128
#define OLED_HEIGHT         64
#define OLED_BUFFER_SIZE    (OLED_WIDTH * OLED_HEIGHT / 8) // 1024 bytes

static i2c_master_bus_handle_t s_i2c_bus = NULL;
static i2c_master_dev_handle_t s_oled_dev = NULL;
static uint8_t s_framebuffer[OLED_BUFFER_SIZE];
static SemaphoreHandle_t s_ui_mutex = NULL;
static ui_display_state_t s_current_state;

static ui_mode_t s_current_ui_mode = UI_MODE_PLAYBACK;
static ui_menu_state_t s_current_menu_state;
static ui_eq_adjust_state_t s_current_eq_state;

// Temporary overlay state
static int s_overlay_vol_timeout = 0;
static int s_overlay_vol_val = 0;
static int s_overlay_eq_timeout = 0;
static char s_overlay_eq_name[16] = {0};

/* 5x7 ASCII Bitmap Font (ASCII 32 ' ' to 126 '~') */
static const uint8_t font5x7[96][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, // ' ' (32)
    {0x00, 0x00, 0x5F, 0x00, 0x00}, // '!'
    {0x00, 0x07, 0x00, 0x07, 0x00}, // '"'
    {0x14, 0x7F, 0x14, 0x7F, 0x14}, // '#'
    {0x24, 0x2A, 0x7F, 0x2A, 0x12}, // '$'
    {0x23, 0x13, 0x08, 0x64, 0x62}, // '%'
    {0x36, 0x49, 0x55, 0x22, 0x50}, // '&'
    {0x00, 0x05, 0x03, 0x00, 0x00}, // '\''
    {0x00, 0x1C, 0x22, 0x41, 0x00}, // '('
    {0x00, 0x41, 0x22, 0x1C, 0x00}, // ')'
    {0x08, 0x2A, 0x1C, 0x2A, 0x08}, // '*'
    {0x08, 0x08, 0x3E, 0x08, 0x08}, // '+'
    {0x00, 0x50, 0x30, 0x00, 0x00}, // ','
    {0x08, 0x08, 0x08, 0x08, 0x08}, // '-'
    {0x00, 0x60, 0x60, 0x00, 0x00}, // '.'
    {0x20, 0x10, 0x08, 0x04, 0x02}, // '/'
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, // '0'
    {0x00, 0x42, 0x7F, 0x40, 0x00}, // '1'
    {0x42, 0x61, 0x51, 0x49, 0x46}, // '2'
    {0x21, 0x41, 0x45, 0x4B, 0x31}, // '3'
    {0x18, 0x14, 0x12, 0x7F, 0x10}, // '4'
    {0x27, 0x45, 0x45, 0x45, 0x39}, // '5'
    {0x3C, 0x4A, 0x49, 0x49, 0x30}, // '6'
    {0x01, 0x71, 0x09, 0x05, 0x03}, // '7'
    {0x36, 0x49, 0x49, 0x49, 0x36}, // '8'
    {0x06, 0x49, 0x49, 0x29, 0x1E}, // '9'
    {0x00, 0x36, 0x36, 0x00, 0x00}, // ':'
    {0x00, 0x56, 0x36, 0x00, 0x00}, // ';'
    {0x08, 0x14, 0x22, 0x41, 0x00}, // '<'
    {0x14, 0x14, 0x14, 0x14, 0x14}, // '='
    {0x00, 0x41, 0x22, 0x14, 0x08}, // '>'
    {0x02, 0x01, 0x51, 0x09, 0x06}, // '?'
    {0x32, 0x49, 0x79, 0x41, 0x3E}, // '@'
    {0x7E, 0x11, 0x11, 0x11, 0x7E}, // 'A'
    {0x7F, 0x49, 0x49, 0x49, 0x36}, // 'B'
    {0x3E, 0x41, 0x41, 0x41, 0x22}, // 'C'
    {0x7F, 0x41, 0x41, 0x22, 0x1C}, // 'D'
    {0x7F, 0x49, 0x49, 0x49, 0x41}, // 'E'
    {0x7F, 0x09, 0x09, 0x09, 0x01}, // 'F'
    {0x3E, 0x41, 0x49, 0x49, 0x7A}, // 'G'
    {0x7F, 0x08, 0x08, 0x08, 0x7F}, // 'H'
    {0x00, 0x41, 0x7F, 0x41, 0x00}, // 'I'
    {0x20, 0x40, 0x41, 0x3F, 0x01}, // 'J'
    {0x7F, 0x08, 0x14, 0x22, 0x41}, // 'K'
    {0x7F, 0x40, 0x40, 0x40, 0x40}, // 'L'
    {0x7F, 0x02, 0x0C, 0x02, 0x7F}, // 'M'
    {0x7F, 0x04, 0x08, 0x10, 0x7F}, // 'N'
    {0x3E, 0x41, 0x41, 0x41, 0x3E}, // 'O'
    {0x7F, 0x09, 0x09, 0x09, 0x06}, // 'P'
    {0x3E, 0x41, 0x51, 0x21, 0x5E}, // 'Q'
    {0x7F, 0x09, 0x19, 0x29, 0x46}, // 'R'
    {0x46, 0x49, 0x49, 0x49, 0x31}, // 'S'
    {0x01, 0x01, 0x7F, 0x01, 0x01}, // 'T'
    {0x3F, 0x40, 0x40, 0x40, 0x3F}, // 'U'
    {0x1F, 0x20, 0x40, 0x20, 0x1F}, // 'V'
    {0x3F, 0x40, 0x38, 0x40, 0x3F}, // 'W'
    {0x63, 0x14, 0x08, 0x14, 0x63}, // 'X'
    {0x07, 0x08, 0x70, 0x08, 0x07}, // 'Y'
    {0x61, 0x51, 0x49, 0x45, 0x43}, // 'Z'
    {0x00, 0x7F, 0x41, 0x41, 0x00}, // '['
    {0x02, 0x04, 0x08, 0x10, 0x20}, // '\'
    {0x00, 0x41, 0x41, 0x7F, 0x00}, // ']'
    {0x04, 0x02, 0x01, 0x02, 0x04}, // '^'
    {0x40, 0x40, 0x40, 0x40, 0x40}, // '_'
    {0x00, 0x01, 0x02, 0x04, 0x00}, // '`'
    {0x20, 0x54, 0x54, 0x54, 0x78}, // 'a'
    {0x7F, 0x48, 0x44, 0x44, 0x38}, // 'b'
    {0x38, 0x44, 0x44, 0x44, 0x20}, // 'c'
    {0x38, 0x44, 0x44, 0x48, 0x7F}, // 'd'
    {0x38, 0x54, 0x54, 0x54, 0x18}, // 'e'
    {0x08, 0x7E, 0x09, 0x01, 0x02}, // 'f'
    {0x0C, 0x52, 0x52, 0x52, 0x3E}, // 'g'
    {0x7F, 0x08, 0x04, 0x04, 0x78}, // 'h'
    {0x00, 0x44, 0x7D, 0x40, 0x00}, // 'i'
    {0x20, 0x40, 0x44, 0x3D, 0x00}, // 'j'
    {0x7F, 0x10, 0x28, 0x44, 0x00}, // 'k'
    {0x00, 0x41, 0x7F, 0x40, 0x00}, // 'l'
    {0x7C, 0x04, 0x18, 0x04, 0x78}, // 'm'
    {0x7C, 0x08, 0x04, 0x04, 0x78}, // 'n'
    {0x38, 0x44, 0x44, 0x44, 0x38}, // 'o'
    {0x7C, 0x14, 0x14, 0x14, 0x08}, // 'p'
    {0x08, 0x14, 0x14, 0x18, 0x7C}, // 'q'
    {0x7C, 0x08, 0x04, 0x04, 0x08}, // 'r'
    {0x48, 0x54, 0x54, 0x54, 0x20}, // 's'
    {0x04, 0x3F, 0x44, 0x40, 0x20}, // 't'
    {0x3C, 0x40, 0x40, 0x20, 0x7C}, // 'u'
    {0x1C, 0x20, 0x40, 0x20, 0x1C}, // 'v'
    {0x3C, 0x40, 0x30, 0x40, 0x3C}, // 'w'
    {0x44, 0x28, 0x10, 0x28, 0x44}, // 'x'
    {0x0C, 0x50, 0x50, 0x50, 0x3C}, // 'y'
    {0x44, 0x64, 0x54, 0x4C, 0x44}, // 'z'
    {0x00, 0x08, 0x36, 0x41, 0x00}, // '{'
    {0x00, 0x00, 0x7F, 0x00, 0x00}, // '|'
    {0x00, 0x41, 0x36, 0x08, 0x00}, // '}'
    {0x08, 0x08, 0x2A, 0x1C, 0x08}, // '~'
    {0x00, 0x00, 0x00, 0x00, 0x00}  // DEL
};

static void oled_clear(void)
{
    memset(s_framebuffer, 0, sizeof(s_framebuffer));
}

static void oled_draw_pixel(int x, int y, bool on)
{
    if (x < 0 || x >= OLED_WIDTH || y < 0 || y >= OLED_HEIGHT) return;
    int page = y / 8;
    int bit = y % 8;
    int idx = page * OLED_WIDTH + x;
    if (on) {
        s_framebuffer[idx] |= (1 << bit);
    } else {
        s_framebuffer[idx] &= ~(1 << bit);
    }
}

static void oled_draw_char(int x, int y, char c, bool invert)
{
    if (c < 32 || c > 126) c = ' ';
    int font_idx = c - 32;

    for (int col = 0; col < 5; col++) {
        int px = x + col;
        if (px >= OLED_WIDTH) break;
        uint8_t line = font5x7[font_idx][col];
        for (int row = 0; row < 7; row++) {
            int py = y + row;
            if (py >= OLED_HEIGHT) break;
            bool on = (line & (1 << row)) != 0;
            oled_draw_pixel(px, py, invert ? !on : on);
        }
    }
    // 1-pixel gap after character
    for (int row = 0; row < 7; row++) {
        oled_draw_pixel(x + 5, y + row, invert ? true : false);
    }
}

static void oled_draw_string(int x, int y, const char *str, bool invert)
{
    if (!str) return;
    int cx = x;
    while (*str && cx < OLED_WIDTH - 5) {
        oled_draw_char(cx, y, *str, invert);
        cx += 6;
        str++;
    }
}

static void oled_draw_hline(int x, int y, int w, bool on)
{
    for (int i = 0; i < w; i++) {
        oled_draw_pixel(x + i, y, on);
    }
}

static void oled_draw_rect(int x, int y, int w, int h, bool fill, bool on)
{
    for (int r = 0; r < h; r++) {
        for (int c = 0; c < w; c++) {
            if (fill || r == 0 || r == h - 1 || c == 0 || c == w - 1) {
                oled_draw_pixel(x + c, y + r, on);
            }
        }
    }
}

static void oled_draw_progress_bar(int x, int y, int w, int h, int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    // Outer border
    oled_draw_rect(x, y, w, h, false, true);
    // Fill inner
    int fill_w = ((w - 4) * percent) / 100;
    if (fill_w > 0) {
        oled_draw_rect(x + 2, y + 2, fill_w, h - 4, true, true);
    }
}

static esp_err_t oled_flush(void)
{
    if (!s_oled_dev) return ESP_FAIL;

    // Send column and page address reset
    const uint8_t addr_cmds[] = {
        0x00,       // Control byte: Command stream
        0x21, 0, 127, // Set Column Address: 0 to 127
        0x22, 0, 7    // Set Page Address: 0 to 7
    };
    esp_err_t ret = i2c_master_transmit(s_oled_dev, addr_cmds, sizeof(addr_cmds), 50);
    if (ret != ESP_OK) return ret;

    // Stream 1024 data bytes in pages or multi-buffer
    // Standard I2C transfer: 0x40 (Data stream) + 1024 bytes
    uint8_t data_chunk[129];
    data_chunk[0] = 0x40; // Data mode
    for (int page = 0; page < 8; page++) {
        memcpy(&data_chunk[1], &s_framebuffer[page * OLED_WIDTH], OLED_WIDTH);
        ret = i2c_master_transmit(s_oled_dev, data_chunk, sizeof(data_chunk), 50);
        if (ret != ESP_OK) return ret;
    }
    return ESP_OK;
}

static void render_playback_screen(void)
{
    xSemaphoreTake(s_ui_mutex, portMAX_DELAY);
    ui_display_state_t state = s_current_state;
    int vol_timeout = s_overlay_vol_timeout;
    int vol_val = s_overlay_vol_val;
    int eq_timeout = s_overlay_eq_timeout;
    char eq_name[16];
    strncpy(eq_name, s_overlay_eq_name, sizeof(eq_name));
    if (s_overlay_vol_timeout > 0) s_overlay_vol_timeout--;
    if (s_overlay_eq_timeout > 0) s_overlay_eq_timeout--;
    xSemaphoreGive(s_ui_mutex);

    oled_clear();

    // 1. Header (y: 0..9)
    char header_left[32];
    snprintf(header_left, sizeof(header_left), "[%s] %s",
             state.mode_str[0] ? state.mode_str : "DSP DAC",
             state.sample_rate_str[0] ? state.sample_rate_str : "48.0k");
    oled_draw_string(0, 0, header_left, false);

    char header_right[16];
    if (state.is_muted) {
        snprintf(header_right, sizeof(header_right), "MUTED");
    } else {
        snprintf(header_right, sizeof(header_right), "V:%d%%", state.volume);
    }
    int right_x = OLED_WIDTH - (int)strlen(header_right) * 6;
    oled_draw_string(right_x, 0, header_right, state.is_muted);

    oled_draw_hline(0, 10, OLED_WIDTH, true);

    // 2. Main Content Body (y: 14..44)
    if (vol_timeout > 0) {
        // Overlay: Large Volume Slider
        oled_draw_rect(10, 16, 108, 30, true, false); // Box clear
        oled_draw_rect(10, 16, 108, 30, false, true); // Box border
        char v_str[16];
        snprintf(v_str, sizeof(v_str), "VOLUME: %d%%", vol_val);
        oled_draw_string(24, 20, v_str, false);
        oled_draw_progress_bar(16, 32, 96, 9, vol_val);
    } else if (eq_timeout > 0) {
        // Overlay: EQ Preset Box
        oled_draw_rect(10, 16, 108, 30, true, false);
        oled_draw_rect(10, 16, 108, 30, false, true);
        oled_draw_string(20, 20, "<< EQ PRESET >>", false);
        oled_draw_string(24, 32, eq_name, false);
    } else {
        // Normal Track / Mode Info
        oled_draw_string(2, 16, state.title[0] ? state.title : "ESP32-S3 Audio DSP", false);
        oled_draw_string(2, 28, state.subtitle[0] ? state.subtitle : "Active", false);

        // Small volume bar on bottom of main body
        oled_draw_progress_bar(2, 40, 124, 6, state.volume);
    }

    // 3. Footer (y: 52..63)
    oled_draw_hline(0, 50, OLED_WIDTH, true);
    char footer_eq[24];
    snprintf(footer_eq, sizeof(footer_eq), "EQ:%s", state.eq_preset[0] ? state.eq_preset : "Flat");
    oled_draw_string(0, 54, footer_eq, false);

    const char *status_str = state.is_muted ? "MUTE" : (state.is_playing ? "PLAY" : "IDLE");
    int status_x = OLED_WIDTH - (int)strlen(status_str) * 6;
    oled_draw_string(status_x, 54, status_str, state.is_muted);

    oled_flush();
}

static void render_menu_screen(void)
{
    xSemaphoreTake(s_ui_mutex, portMAX_DELAY);
    ui_menu_state_t menu = s_current_menu_state;
    xSemaphoreGive(s_ui_mutex);

    oled_clear();

    // 1. Header (y: 0..9)
    oled_draw_string(18, 0, "== MAIN MENU ==", false);
    oled_draw_hline(0, 10, OLED_WIDTH, true);

    // 2. Menu items (Visible window of 4 items)
    int top_idx = 0;
    if (menu.selected_idx >= 3) {
        top_idx = menu.selected_idx - 2;
        if (top_idx > MENU_ITEM_COUNT - 4) {
            top_idx = MENU_ITEM_COUNT - 4;
        }
    }

    char line_buf[48];
    for (int i = 0; i < 4; i++) {
        int item_idx = top_idx + i;
        if (item_idx >= MENU_ITEM_COUNT) break;

        switch (item_idx) {
        case MENU_ITEM_EQ_ADJUST:
            snprintf(line_buf, sizeof(line_buf), "1. Live EQ Adjust");
            break;
        case MENU_ITEM_EQ_PRESET:
            snprintf(line_buf, sizeof(line_buf), "2. Preset:%s", menu.current_preset[0] ? menu.current_preset : "Flat");
            break;
        case MENU_ITEM_AUDIO_SOURCE:
            snprintf(line_buf, sizeof(line_buf), "3. Src:%s", menu.current_source[0] ? menu.current_source : "USB DAC");
            break;
        case MENU_ITEM_PREAMP_GAIN:
            snprintf(line_buf, sizeof(line_buf), "4. Preamp:%.1fdB", menu.preamp_gain_db);
            break;
        case MENU_ITEM_SAVE_NVS:
            snprintf(line_buf, sizeof(line_buf), "5. Save to NVS");
            break;
        case MENU_ITEM_EXIT:
            snprintf(line_buf, sizeof(line_buf), "6. Exit Menu");
            break;
        default:
            snprintf(line_buf, sizeof(line_buf), " ");
            break;
        }

        bool is_selected = (item_idx == menu.selected_idx);
        int y_pos = 13 + (i * 9);
        if (is_selected) {
            oled_draw_rect(0, y_pos - 1, OLED_WIDTH, 9, true, true);
            oled_draw_string(2, y_pos, line_buf, true);
        } else {
            oled_draw_string(2, y_pos, line_buf, false);
        }
    }

    // 3. Footer (y: 52..63)
    oled_draw_hline(0, 50, OLED_WIDTH, true);
    oled_draw_string(2, 54, "Rot:Move  SW:Select", false);

    oled_flush();
}

static void render_eq_adjust_screen(void)
{
    xSemaphoreTake(s_ui_mutex, portMAX_DELAY);
    ui_eq_adjust_state_t eq = s_current_eq_state;
    xSemaphoreGive(s_ui_mutex);

    oled_clear();

    // 1. Header (y: 0..8): "BND:[3]/5  [PEAK]"
    char band_str[8];
    snprintf(band_str, sizeof(band_str), "[%d]", eq.active_band + 1);
    char total_str[8];
    snprintf(total_str, sizeof(total_str), "/%d", eq.num_bands);

    oled_draw_string(2, 0, "BND:", false);
    bool bnd_sel = (eq.cursor == EQ_CURSOR_BAND);
    oled_draw_string(26, 0, band_str, bnd_sel);
    oled_draw_string(26 + (int)strlen(band_str) * 6, 0, total_str, false);

    const char *type_str = "PEAK";
    if (eq.band.type == PEQ_FILTER_LOW_SHELF) type_str = "L-SH";
    else if (eq.band.type == PEQ_FILTER_HIGH_SHELF) type_str = "H-SH";
    else if (eq.band.type == PEQ_FILTER_BYPASS) type_str = "BYP";

    char type_box[12];
    snprintf(type_box, sizeof(type_box), "[%s]", type_str);
    bool type_sel = (eq.cursor == EQ_CURSOR_TYPE);
    oled_draw_string(80, 0, type_box, type_sel);

    oled_draw_hline(0, 9, OLED_WIDTH, true);

    // 2. Frequency (y: 11..19): "FREQ:  0 1 0 0 0  Hz"
    int f_int = (int)roundf(eq.band.freq_hz);
    if (f_int < 20) f_int = 20;
    if (f_int > 20000) f_int = 20000;
    int f_10k = (f_int / 10000) % 10;
    int f_1k  = (f_int / 1000) % 10;
    int f_100 = (f_int / 100) % 10;
    int f_10  = (f_int / 10) % 10;
    int f_1   = f_int % 10;

    oled_draw_string(2, 11, "FREQ:", false);
    oled_draw_char(38, 11, '0' + f_10k, eq.cursor == EQ_CURSOR_FREQ_10K);
    oled_draw_char(46, 11, '0' + f_1k,  eq.cursor == EQ_CURSOR_FREQ_1K);
    oled_draw_char(54, 11, '0' + f_100, eq.cursor == EQ_CURSOR_FREQ_100);
    oled_draw_char(62, 11, '0' + f_10,  eq.cursor == EQ_CURSOR_FREQ_10);
    oled_draw_char(70, 11, '0' + f_1,   eq.cursor == EQ_CURSOR_FREQ_1);
    oled_draw_string(82, 11, "Hz", false);

    // 3. Gain (y: 21..29): "GAIN:  + 0 4 . 5  dB"
    float g = eq.band.gain_db;
    char g_sign = (g >= 0.0f) ? '+' : '-';
    float g_abs = fabsf(g);
    int g_tens = ((int)g_abs / 10) % 10;
    int g_ones = ((int)g_abs) % 10;
    int g_dec  = ((int)(g_abs * 10.0f + 0.5f)) % 10;

    oled_draw_string(2, 21, "GAIN:", false);
    oled_draw_char(38, 21, g_sign,       eq.cursor == EQ_CURSOR_GAIN_SIGN);
    oled_draw_char(46, 21, '0' + g_tens, eq.cursor == EQ_CURSOR_GAIN_10);
    oled_draw_char(54, 21, '0' + g_ones, eq.cursor == EQ_CURSOR_GAIN_1);
    oled_draw_char(60, 21, '.',          false);
    oled_draw_char(66, 21, '0' + g_dec,  eq.cursor == EQ_CURSOR_GAIN_DEC);
    oled_draw_string(82, 21, "dB", false);

    // 4. Q Factor (y: 31..39): "Q   :  0 1 . 4 1"
    float q = eq.band.q_factor;
    if (q < 0.10f) q = 0.10f;
    if (q > 10.00f) q = 10.00f;
    int q_int  = (int)q;
    int q_dec1 = ((int)(q * 10.0f + 0.05f)) % 10;
    int q_dec2 = ((int)(q * 100.0f + 0.5f)) % 10;

    oled_draw_string(2, 31, "Q   :", false);
    oled_draw_char(46, 31, '0' + q_int,  eq.cursor == EQ_CURSOR_Q_INT);
    oled_draw_char(54, 31, '.',          false);
    oled_draw_char(60, 31, '0' + q_dec1, eq.cursor == EQ_CURSOR_Q_DEC1);
    oled_draw_char(68, 31, '0' + q_dec2, eq.cursor == EQ_CURSOR_Q_DEC2);

    // 5. Hint text (y: 41..48)
    oled_draw_string(2, 42, "< >:Move  Rot:0-9", false);

    // 6. Action Toolbar (y: 52..63)
    oled_draw_hline(0, 50, OLED_WIDTH, true);
    oled_draw_string(2,   53, "[+B]",   eq.cursor == EQ_CURSOR_ACTION_ADD);
    oled_draw_string(32,  53, "[-B]",   eq.cursor == EQ_CURSOR_ACTION_DEL);
    oled_draw_string(62,  53, "[SAVE]", eq.cursor == EQ_CURSOR_ACTION_SAVE);
    oled_draw_string(101, 53, "[EXIT]", eq.cursor == EQ_CURSOR_ACTION_EXIT);

    // 7. Saved notification overlay popup
    if (eq.saved_notification) {
        oled_draw_rect(14, 18, 100, 26, true, false); // clear black box
        oled_draw_rect(14, 18, 100, 26, false, true); // white border
        oled_draw_string(22, 27, "SAVED TO NVS!", false);
    }

    oled_flush();
}

static void render_screen(void)
{
    ui_mode_t mode;
    xSemaphoreTake(s_ui_mutex, portMAX_DELAY);
    mode = s_current_ui_mode;
    xSemaphoreGive(s_ui_mutex);

    switch (mode) {
    case UI_MODE_MENU:
        render_menu_screen();
        break;
    case UI_MODE_EQ_ADJUST:
        render_eq_adjust_screen();
        break;
    case UI_MODE_PLAYBACK:
    default:
        render_playback_screen();
        break;
    }
}

static void oled_ui_task(void *pvParameters)
{
    ESP_LOGI(TAG, "OLED UI rendering task started on Core 0.");
    while (1) {
        render_screen();
        vTaskDelay(pdMS_TO_TICKS(100)); // 10 FPS smooth display update
    }
}

esp_err_t oled_display_init(void)
{
    ESP_LOGI(TAG, "Initializing I2C Master bus on SDA=%d, SCL=%d...",
             BOARD_I2C_SDA_PIN, BOARD_I2C_SCL_PIN);

    s_ui_mutex = xSemaphoreCreateMutex();
    if (!s_ui_mutex) return ESP_ERR_NO_MEM;

    // Default boot state
    memset(&s_current_state, 0, sizeof(s_current_state));
    strncpy(s_current_state.mode_str, "USB DAC", sizeof(s_current_state.mode_str));
    strncpy(s_current_state.sample_rate_str, "48.0k 16b", sizeof(s_current_state.sample_rate_str));
    strncpy(s_current_state.title, "ESP32-S3 Audio DSP", sizeof(s_current_state.title));
    strncpy(s_current_state.subtitle, "Ready / Standalone", sizeof(s_current_state.subtitle));
    strncpy(s_current_state.eq_preset, "Flat", sizeof(s_current_state.eq_preset));
    s_current_state.volume = 80;
    s_current_state.is_playing = true;

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = (gpio_num_t)BOARD_I2C_SDA_PIN,
        .scl_io_num = (gpio_num_t)BOARD_I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t ret = i2c_new_master_bus(&bus_cfg, &s_i2c_bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2C bus: %s", esp_err_to_name(ret));
        return ret;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = I2C_OLED_ADDR_DEFAULT, // Standard 0x3C
        .scl_speed_hz = 400000,                  // 400 kHz Fast-mode
    };
    ret = i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_oled_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to attach SSD1306 device: %s", esp_err_to_name(ret));
        return ret;
    }

    // SSD1306 Power-On Command Sequence
    const uint8_t init_cmds[] = {
        0x00,       // Control byte: Command stream
        0xAE,       // Display OFF
        0xD5, 0x80, // Set display clock divide ratio/oscillator
        0xA8, 0x3F, // Set multiplex ratio (64 lines)
        0xD3, 0x00, // Set display offset (0)
        0x40,       // Set start line (0)
        0x8D, 0x14, // Enable internal charge pump regulator
        0x20, 0x00, // Set horizontal addressing mode
        0xA1,       // Set segment re-map (col 127 mapped to SEG0)
        0xC8,       // Set COM output scan direction (remapped)
        0xDA, 0x12, // Set COM pins hardware configuration
        0x81, 0xCF, // Set contrast control (0xCF)
        0xD9, 0xF1, // Set pre-charge period
        0xDB, 0x40, // Set VCOMH deselect level
        0xA4,       // Entire display ON from RAM
        0xA6,       // Normal display (non-inverted)
        0xAF        // Display ON
    };
    ret = i2c_master_transmit(s_oled_dev, init_cmds, sizeof(init_cmds), 100);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SSD1306 transmission warning: %s (check display connected on 0x3C)", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "SSD1306 128x64 OLED display initialized successfully at address 0x%02X.", I2C_OLED_ADDR_DEFAULT);
    }

    // Initialize default Menu and EQ state
    s_current_menu_state.selected_idx = 0;
    strncpy(s_current_menu_state.current_preset, "Flat", sizeof(s_current_menu_state.current_preset));
    strncpy(s_current_menu_state.current_source, "USB DAC", sizeof(s_current_menu_state.current_source));
    s_current_menu_state.preamp_gain_db = 0.0f;

    s_current_eq_state.active_band = 0;
    s_current_eq_state.num_bands = 5;
    s_current_eq_state.cursor = EQ_CURSOR_FREQ_1K;
    s_current_eq_state.band.type = PEQ_FILTER_PEAKING;
    s_current_eq_state.band.freq_hz = 1000.0f;
    s_current_eq_state.band.gain_db = 0.0f;
    s_current_eq_state.band.q_factor = 1.0f;
    s_current_eq_state.band.enabled = true;
    s_current_eq_state.saved_notification = false;

    // Start UI render task on Core 0 (Priority 2, Stack 4096 bytes)
    // Strictly follows Invariant 9 (Core 0 = UI/Network, Core 1 = Audio DSP)
    xTaskCreatePinnedToCore(oled_ui_task, "oled_ui_task", 4096, NULL, 2, NULL, 0);

    return ESP_OK;
}

void oled_display_set_state(const ui_display_state_t *state)
{
    if (!state || !s_ui_mutex) return;
    xSemaphoreTake(s_ui_mutex, portMAX_DELAY);
    s_current_state = *state;
    xSemaphoreGive(s_ui_mutex);
}

void oled_display_show_volume_overlay(int volume)
{
    if (!s_ui_mutex) return;
    xSemaphoreTake(s_ui_mutex, portMAX_DELAY);
    s_overlay_vol_timeout = 15; // 1.5 seconds at 10 FPS
    s_overlay_vol_val = volume;
    s_current_state.volume = volume;
    xSemaphoreGive(s_ui_mutex);
}

void oled_display_show_eq_overlay(const char *preset_name)
{
    if (!s_ui_mutex || !preset_name) return;
    xSemaphoreTake(s_ui_mutex, portMAX_DELAY);
    s_overlay_eq_timeout = 20; // 2.0 seconds at 10 FPS
    strncpy(s_overlay_eq_name, preset_name, sizeof(s_overlay_eq_name) - 1);
    strncpy(s_current_state.eq_preset, preset_name, sizeof(s_current_state.eq_preset) - 1);
    xSemaphoreGive(s_ui_mutex);
}

void oled_display_set_ui_mode(ui_mode_t mode)
{
    if (!s_ui_mutex) return;
    xSemaphoreTake(s_ui_mutex, portMAX_DELAY);
    s_current_ui_mode = mode;
    xSemaphoreGive(s_ui_mutex);
}

ui_mode_t oled_display_get_ui_mode(void)
{
    if (!s_ui_mutex) return UI_MODE_PLAYBACK;
    xSemaphoreTake(s_ui_mutex, portMAX_DELAY);
    ui_mode_t mode = s_current_ui_mode;
    xSemaphoreGive(s_ui_mutex);
    return mode;
}

void oled_display_update_menu(int selected_index, const char *preset_name, const char *source_name, float preamp_gain_db)
{
    if (!s_ui_mutex) return;
    xSemaphoreTake(s_ui_mutex, portMAX_DELAY);
    s_current_menu_state.selected_idx = selected_index;
    if (preset_name) {
        strncpy(s_current_menu_state.current_preset, preset_name, sizeof(s_current_menu_state.current_preset) - 1);
    }
    if (source_name) {
        strncpy(s_current_menu_state.current_source, source_name, sizeof(s_current_menu_state.current_source) - 1);
    }
    s_current_menu_state.preamp_gain_db = preamp_gain_db;
    xSemaphoreGive(s_ui_mutex);
}

void oled_display_update_eq_adjust(const ui_eq_adjust_state_t *eq_state)
{
    if (!s_ui_mutex || !eq_state) return;
    xSemaphoreTake(s_ui_mutex, portMAX_DELAY);
    s_current_eq_state = *eq_state;
    xSemaphoreGive(s_ui_mutex);
}
