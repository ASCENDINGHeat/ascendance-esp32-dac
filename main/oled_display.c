#include "oled_display.h"
#include "board_pins.h"
#include "webhid_peq.h"

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
#define OLED_I2C_TIMEOUT_MS 10
#define OLED_BLANK_SEC      180U

static i2c_master_bus_handle_t s_i2c_bus = NULL;
static i2c_master_dev_handle_t s_oled_dev = NULL;
static uint8_t s_oled_addr = I2C_OLED_ADDR_DEFAULT;
static bool s_oled_online = false;
static uint8_t s_framebuffer[OLED_BUFFER_SIZE];
static SemaphoreHandle_t s_ui_mutex = NULL;
static ui_display_state_t s_current_state;

static ui_mode_t s_current_ui_mode = UI_MODE_PLAYBACK;
static ui_menu_state_t s_current_menu_state;
static ui_eq_adjust_state_t s_current_eq_state;
static oled_playback_view_t s_playback_view = OLED_VIEW_HUD;

// Temporary overlay state
static int s_overlay_vol_timeout = 0;
static int s_overlay_vol_val = 0;
static int s_overlay_eq_timeout = 0;
static char s_overlay_eq_name[24] = {0};

// EQ Curve cache (evaluated on Core 0 when invalidated)
static float s_curve_db[OLED_WIDTH];
static bool  s_curve_dirty = true;

// VU 300ms peak-hold state
static float s_vu_hold_db_l = -60.0f;
static float s_vu_hold_db_r = -60.0f;
static int   s_vu_hold_ticks_l = 0;
static int   s_vu_hold_ticks_r = 0;

// Burn-in guard & power state
static TickType_t s_last_activity_tick = 0;
static uint16_t   s_dim_sec = 60;
static bool       s_is_dimmed = false;
static bool       s_is_blanked = false;
static bool       s_usb_suspended = false;
static int        s_shift_x = 0;
static int        s_shift_y = 0;
static int        s_probe_tick_cnt = 0;
static int        s_frame_tick_cnt = 0;


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
    int sx = x + (s_is_dimmed ? s_shift_x : 0);
    int sy = y + (s_is_dimmed ? s_shift_y : 0);
    if (sx < 0 || sx >= OLED_WIDTH || sy < 0 || sy >= OLED_HEIGHT) return;
    int page = sy / 8;
    int bit = sy % 8;
    int idx = page * OLED_WIDTH + sx;
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

static void oled_draw_vline(int x, int y, int h, bool on)
{
    for (int i = 0; i < h; i++) {
        oled_draw_pixel(x, y + i, on);
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
    oled_draw_rect(x, y, w, h, false, true);
    int fill_w = ((w - 4) * percent) / 100;
    if (fill_w > 0) {
        oled_draw_rect(x + 2, y + 2, fill_w, h - 4, true, true);
    }
}

static esp_err_t ssd1306_send_cmd2(uint8_t c1, uint8_t c2)
{
    if (!s_oled_dev || !s_oled_online) return ESP_ERR_INVALID_STATE;
    const uint8_t cmds[3] = {0x00, c1, c2};
    return i2c_master_transmit(s_oled_dev, cmds, sizeof(cmds), OLED_I2C_TIMEOUT_MS);
}

static esp_err_t ssd1306_send_cmd1(uint8_t c1)
{
    if (!s_oled_dev || !s_oled_online) return ESP_ERR_INVALID_STATE;
    const uint8_t cmds[2] = {0x00, c1};
    return i2c_master_transmit(s_oled_dev, cmds, sizeof(cmds), OLED_I2C_TIMEOUT_MS);
}

static esp_err_t ssd1306_probe_and_init(void)
{
    if (!s_i2c_bus) return ESP_ERR_INVALID_STATE;

    const uint8_t addrs[2] = { I2C_OLED_ADDR_DEFAULT, I2C_OLED_ADDR_ALT };
    uint8_t found_addr = 0;
    for (int i = 0; i < 2; i++) {
        if (i2c_master_probe(s_i2c_bus, addrs[i], OLED_I2C_TIMEOUT_MS) == ESP_OK) {
            found_addr = addrs[i];
            break;
        }
    }
    if (found_addr == 0) {
        s_oled_online = false;
        return ESP_ERR_NOT_FOUND;
    }

    if (!s_oled_dev || s_oled_addr != found_addr) {
        if (s_oled_dev) {
            i2c_master_bus_rm_device(s_oled_dev);
            s_oled_dev = NULL;
        }
        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = found_addr,
            .scl_speed_hz = 400000,
        };
        esp_err_t add_ret = i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_oled_dev);
        if (add_ret != ESP_OK) {
            s_oled_online = false;
            return add_ret;
        }
        s_oled_addr = found_addr;
    }

    const uint8_t init_cmds[] = {
        0x00,       // Command stream
        0xAE,       // Display OFF
        0xD5, 0x80, // Display clock divide ratio
        0xA8, 0x3F, // Multiplex ratio (64)
        0xD3, 0x00, // Display offset (0)
        0x40,       // Start line (0)
        0x8D, 0x14, // Charge pump ON
        0x20, 0x00, // Horizontal addressing mode
        0xA1,       // Segment re-map
        0xC8,       // COM scan remapped
        0xDA, 0x12, // COM pins
        0x81, 0xCF, // Contrast control (normal)
        0xD9, 0xF1, // Pre-charge
        0xDB, 0x40, // VCOMH deselect
        0xA4,       // RAM resume
        0xA6,       // Normal display
        0xAF        // Display ON
    };
    esp_err_t ret = i2c_master_transmit(s_oled_dev, init_cmds, sizeof(init_cmds), OLED_I2C_TIMEOUT_MS);
    if (ret == ESP_OK) {
        if (!s_oled_online) {
            ESP_LOGI(TAG, "SSD1306 OLED online at I2C addr 0x%02X.", found_addr);
        }
        s_oled_online = true;
        s_is_dimmed = false;
        s_is_blanked = false;
    } else {
        s_oled_online = false;
    }
    return ret;
}

static esp_err_t oled_flush(void)
{
    if (!s_oled_dev || !s_oled_online) return ESP_FAIL;

    const uint8_t addr_cmds[] = {
        0x00,
        0x21, 0, 127,
        0x22, 0, 7
    };
    esp_err_t ret = i2c_master_transmit(s_oled_dev, addr_cmds, sizeof(addr_cmds), OLED_I2C_TIMEOUT_MS);
    if (ret != ESP_OK) {
        s_oled_online = false;
        return ret;
    }

    uint8_t data_chunk[129];
    data_chunk[0] = 0x40;
    for (int page = 0; page < 8; page++) {
        memcpy(&data_chunk[1], &s_framebuffer[page * OLED_WIDTH], OLED_WIDTH);
        ret = i2c_master_transmit(s_oled_dev, data_chunk, sizeof(data_chunk), OLED_I2C_TIMEOUT_MS);
        if (ret != ESP_OK) {
            s_oled_online = false;
            return ret;
        }
    }
    return ESP_OK;
}

static void refresh_curve_cache_if_dirty(void)
{
    if (s_curve_dirty) {
        peq_dsp_eval_curve_db(s_curve_db, OLED_WIDTH, 20.0f, 20000.0f);
        s_curve_dirty = false;
    }
}

static int db_to_curve_y(float db)
{
    // Body area y=12..48 (0 dB centerline at y=30, +/-12 dB -> 1.4 px/dB)
    if (db > 12.5f) db = 12.5f;
    if (db < -12.5f) db = -12.5f;
    int y = 30 - (int)lroundf(db * 1.4f);
    if (y < 12) y = 12;
    if (y > 48) y = 48;
    return y;
}

static void draw_curve_plot(int x0, int width)
{
    refresh_curve_cache_if_dirty();

    // Dotted 0 dB centerline
    for (int x = 0; x < width; x++) {
        if ((x & 3) == 0) {
            oled_draw_pixel(x0 + x, 30, true);
        }
    }

    int prev_y = -1;
    for (int x = 0; x < width; x++) {
        int idx = (width > 1) ? ((x * (OLED_WIDTH - 1)) / (width - 1)) : 0;
        int cy = db_to_curve_y(s_curve_db[idx]);
        if (prev_y < 0) prev_y = cy;
        int y_lo = (prev_y < cy) ? prev_y : cy;
        int y_hi = (prev_y > cy) ? prev_y : cy;
        for (int yy = y_lo; yy <= y_hi; yy++) {
            oled_draw_pixel(x0 + x, yy, true);
        }
        prev_y = cy;
    }
}

static void update_vu_hold(const peq_vu_metrics_t *vu)
{
    if (vu->peak_db_l >= s_vu_hold_db_l) {
        s_vu_hold_db_l = vu->peak_db_l;
        s_vu_hold_ticks_l = 3; // 300 ms hold at 10 FPS
    } else if (s_vu_hold_ticks_l > 0) {
        s_vu_hold_ticks_l--;
    } else {
        s_vu_hold_db_l -= 2.2f;
        if (s_vu_hold_db_l < -60.0f) s_vu_hold_db_l = -60.0f;
    }

    if (vu->peak_db_r >= s_vu_hold_db_r) {
        s_vu_hold_db_r = vu->peak_db_r;
        s_vu_hold_ticks_r = 3;
    } else if (s_vu_hold_ticks_r > 0) {
        s_vu_hold_ticks_r--;
    } else {
        s_vu_hold_db_r -= 2.2f;
        if (s_vu_hold_db_r < -60.0f) s_vu_hold_db_r = -60.0f;
    }
}

static int db_to_bar_px(float db, int max_px)
{
    if (db <= -60.0f) return 0;
    if (db >= 0.0f) return max_px;
    int px = (int)lroundf(((db + 60.0f) / 60.0f) * (float)max_px);
    if (px < 0) px = 0;
    if (px > max_px) px = max_px;
    return px;
}

static void render_vu_body(void)
{
    peq_vu_metrics_t vu;
    peq_dsp_get_vu_metrics(&vu, false);
    update_vu_hold(&vu);

    const int bar_x = 12;
    const int bar_w = 88; // x=12..99

    oled_draw_string(2, 15, "L", false);
    oled_draw_rect(bar_x, 15, bar_w, 8, false, true);
    int rms_w_l = db_to_bar_px(vu.rms_db_l, bar_w - 4);
    int pk_w_l  = db_to_bar_px(vu.peak_db_l, bar_w - 4);
    int hd_x_l  = db_to_bar_px(s_vu_hold_db_l, bar_w - 5);
    if (rms_w_l > 0) oled_draw_rect(bar_x + 2, 17, rms_w_l, 4, true, true);
    if (pk_w_l > rms_w_l) oled_draw_hline(bar_x + 2 + rms_w_l, 19, pk_w_l - rms_w_l, true);
    if (hd_x_l > 0) oled_draw_vline(bar_x + 2 + hd_x_l, 16, 6, true);

    oled_draw_string(2, 26, "R", false);
    oled_draw_rect(bar_x, 26, bar_w, 8, false, true);
    int rms_w_r = db_to_bar_px(vu.rms_db_r, bar_w - 4);
    int pk_w_r  = db_to_bar_px(vu.peak_db_r, bar_w - 4);
    int hd_x_r  = db_to_bar_px(s_vu_hold_db_r, bar_w - 5);
    if (rms_w_r > 0) oled_draw_rect(bar_x + 2, 28, rms_w_r, 4, true, true);
    if (pk_w_r > rms_w_r) oled_draw_hline(bar_x + 2 + rms_w_r, 30, pk_w_r - rms_w_r, true);
    if (hd_x_r > 0) oled_draw_vline(bar_x + 2 + hd_x_r, 27, 6, true);

    if (vu.clip_latched) {
        oled_draw_string(103, 15, "CLIP", true);
    } else if (vu.limiter_active) {
        oled_draw_string(106, 15, "LIM", true);
    } else {
        oled_draw_string(106, 15, "0dB", false);
    }
    oled_draw_string(104, 26, peq_dsp_is_limiter_enabled() ? "L:ON" : "L:--", false);

    // Scale legend & Peak readout
    char db_str[28];
    if (peq_dsp_is_signal_silent()) {
        snprintf(db_str, sizeof(db_str), "SILENCE AUTO-MUTE");
    } else {
        float max_pk = (vu.peak_db_l > vu.peak_db_r) ? vu.peak_db_l : vu.peak_db_r;
        if (max_pk < -90.0f) {
            snprintf(db_str, sizeof(db_str), "-60  -36  -18  -6  0");
        } else {
            snprintf(db_str, sizeof(db_str), "PK:%5.1fdB  RMS:%5.1f", max_pk,
                     0.5f * (vu.rms_db_l + vu.rms_db_r));
        }
    }
    oled_draw_string(2, 39, db_str, false);
}

static void render_split_body(void)
{
    peq_vu_metrics_t vu;
    peq_dsp_get_vu_metrics(&vu, false);
    update_vu_hold(&vu);

    // Left 94px: Frequency Response Curve
    draw_curve_plot(0, 94);
    oled_draw_vline(95, 12, 37, true);

    // Right 30px (x=98..127): Vertical L & R VU Bars
    const int vh = 26;
    oled_draw_rect(99, 13, 11, vh + 2, false, true);
    oled_draw_rect(114, 13, 11, vh + 2, false, true);

    int rms_hl = db_to_bar_px(vu.rms_db_l, vh - 2);
    int pk_hl  = db_to_bar_px(s_vu_hold_db_l, vh - 2);
    if (rms_hl > 0) {
        oled_draw_rect(101, 13 + vh - rms_hl, 7, rms_hl, true, true);
    }
    if (pk_hl > 0) {
        oled_draw_hline(101, 13 + vh - pk_hl, 7, true);
    }

    int rms_hr = db_to_bar_px(vu.rms_db_r, vh - 2);
    int pk_hr  = db_to_bar_px(s_vu_hold_db_r, vh - 2);
    if (rms_hr > 0) {
        oled_draw_rect(116, 13 + vh - rms_hr, 7, rms_hr, true, true);
    }
    if (pk_hr > 0) {
        oled_draw_hline(116, 13 + vh - pk_hr, 7, true);
    }

    oled_draw_string(102, 42, "L", vu.clip_latched);
    oled_draw_string(117, 42, "R", vu.limiter_active);
}

static void render_playback_screen(void)
{
    xSemaphoreTake(s_ui_mutex, portMAX_DELAY);
    ui_display_state_t state = s_current_state;
    oled_playback_view_t view = s_playback_view;
    int vol_timeout = s_overlay_vol_timeout;
    int vol_val = s_overlay_vol_val;
    int eq_timeout = s_overlay_eq_timeout;
    char eq_name[24];
    snprintf(eq_name, sizeof(eq_name), "%.23s", s_overlay_eq_name);
    if (s_overlay_vol_timeout > 0) s_overlay_vol_timeout--;
    if (s_overlay_eq_timeout > 0) s_overlay_eq_timeout--;
    xSemaphoreGive(s_ui_mutex);

    oled_clear();

    // 1. Header (y: 0..9)
    char header_left[32];
    snprintf(header_left, sizeof(header_left), "[%s] %s",
             state.mode_str[0] ? state.mode_str : "USB DAC",
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

    // 2. Main Content Body (y: 12..48)
    if (vol_timeout > 0) {
        oled_draw_rect(10, 16, 108, 30, true, false);
        oled_draw_rect(10, 16, 108, 30, false, true);
        char v_str[16];
        snprintf(v_str, sizeof(v_str), "VOLUME: %d%%", vol_val);
        oled_draw_string(24, 20, v_str, false);
        oled_draw_progress_bar(16, 32, 96, 9, vol_val);
    } else if (eq_timeout > 0) {
        oled_draw_rect(10, 16, 108, 30, true, false);
        oled_draw_rect(10, 16, 108, 30, false, true);
        oled_draw_string(19, 20, "<< EQ SLOT >>", false);
        int name_len = (int)strlen(eq_name);
        if (name_len > 17) {
            eq_name[17] = '\0';
            name_len = 17;
        }
        int eq_x = 10 + (108 - name_len * 6) / 2;
        oled_draw_string(eq_x, 32, eq_name, false);
    } else {
        switch (view) {
        case OLED_VIEW_CURVE:
            draw_curve_plot(0, OLED_WIDTH);
            break;
        case OLED_VIEW_VU:
            render_vu_body();
            break;
        case OLED_VIEW_SPLIT:
            render_split_body();
            break;
        case OLED_VIEW_HUD:
        default:
            oled_draw_string(2, 16, state.title[0] ? state.title : "ASCENDANCE Hi-Fi", false);
            oled_draw_string(2, 28, state.subtitle[0] ? state.subtitle : "Active", false);
            oled_draw_progress_bar(2, 40, 124, 6, state.volume);
            break;
        }
    }

    // 3. Footer (y: 52..63)
    oled_draw_hline(0, 50, OLED_WIDTH, true);
    char footer_eq[24];
    snprintf(footer_eq, sizeof(footer_eq), "%.14s", state.eq_preset[0] ? state.eq_preset : "Slot 1");
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

    const char *xf_names[3]   = { "Off", "Low", "High" };
    const char *loud_names[3] = { "Off", "Mild", "Full" };
    const char *view_names[4] = { "Classic", "Curve", "VU", "Split" };
    const char *exc_names[PEQ_EXCITER_MODE_COUNT] = { "Tube", "Tape", "Air" };

    char line_buf[64];
    for (int i = 0; i < 4; i++) {
        int item_idx = top_idx + i;
        if (item_idx >= MENU_ITEM_COUNT) break;

        switch (item_idx) {
        case MENU_ITEM_EQ_ADJUST:
            snprintf(line_buf, sizeof(line_buf), "1. Live EQ Adjust");
            break;
        case MENU_ITEM_EQ_PRESET:
            snprintf(line_buf, sizeof(line_buf), "2. Slot:%.12s",
                     menu.current_preset[0] ? menu.current_preset : "Slot 1");
            break;
        case MENU_ITEM_AB_BYPASS:
            snprintf(line_buf, sizeof(line_buf), "3. A/B:%s", menu.ab_bypass ? "Bypass" : "Active");
            break;
        case MENU_ITEM_CROSSFEED: {
            uint8_t xf = (menu.crossfeed_level < 3) ? menu.crossfeed_level : 0;
            snprintf(line_buf, sizeof(line_buf), "4. Crossfeed:%s", xf_names[xf]);
            break;
        }
        case MENU_ITEM_LOUDNESS: {
            uint8_t ld = (menu.loudness_mode < 3) ? menu.loudness_mode : 0;
            snprintf(line_buf, sizeof(line_buf), "5. Loudness:%s", loud_names[ld]);
            break;
        }
        case MENU_ITEM_LIMITER:
            snprintf(line_buf, sizeof(line_buf), "6. Limiter:%s", menu.limiter_enabled ? "On" : "Off");
            break;
        case MENU_ITEM_OLED_VIEW: {
            uint8_t vw = (menu.oled_view < 4) ? menu.oled_view : 0;
            snprintf(line_buf, sizeof(line_buf), "7. View:%s", view_names[vw]);
            break;
        }
        case MENU_ITEM_BASS_ENH:
            snprintf(line_buf, sizeof(line_buf), "8. Bass Enh:%s", menu.bass_enabled ? "On" : "Off");
            break;
        case MENU_ITEM_EXCITER: {
            uint8_t em = (menu.exciter_mode < PEQ_EXCITER_MODE_COUNT) ? menu.exciter_mode : 0;
            snprintf(line_buf, sizeof(line_buf), "9. Exciter:%s",
                     menu.exciter_enabled ? exc_names[em] : "Off");
            break;
        }
        case MENU_ITEM_PREAMP_GAIN:
            snprintf(line_buf, sizeof(line_buf), "10. Preamp:%.1fdB", menu.preamp_gain_db);
            break;
        case MENU_ITEM_AUTO_PREAMP:
            snprintf(line_buf, sizeof(line_buf), "11. AutoPreamp:%s", menu.auto_preamp_enabled ? "On" : "Off");
            break;
        case MENU_ITEM_AUDIO_SOURCE:
            snprintf(line_buf, sizeof(line_buf), "12. Input:%.10s",
                     menu.current_source[0] ? menu.current_source : "USB DAC");
            break;
        case MENU_ITEM_SAVE_NVS:
            snprintf(line_buf, sizeof(line_buf), "13. Save to DAC");
            break;
        case MENU_ITEM_EXIT:
            snprintf(line_buf, sizeof(line_buf), "14. Exit Menu");
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
    oled_draw_string(2, 54, "Turn:Move  Click:OK", false);

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
    else if (eq.band.type == PEQ_FILTER_HIGH_PASS) type_str = "HPF ";
    else if (eq.band.type == PEQ_FILTER_LOW_PASS) type_str = "LPF ";
    else if (eq.band.type == PEQ_FILTER_NOTCH) type_str = "NTCH";
    else if (eq.band.type == PEQ_FILTER_BAND_PASS) type_str = "BPF ";
    else if (eq.band.type == PEQ_FILTER_BYPASS) type_str = "BYP ";

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
    if (g < -24.0f) g = -24.0f;
    if (g > 12.0f) g = 12.0f;
    int g_x10 = (int)roundf(fabsf(g) * 10.0f);
    char g_sign = (g_x10 == 0 || g >= 0.0f) ? '+' : '-';
    int g_tens = (g_x10 / 100) % 10;
    int g_ones = (g_x10 / 10) % 10;
    int g_dec  = g_x10 % 10;

    oled_draw_string(2, 21, "GAIN:", false);
    oled_draw_char(38, 21, g_sign,       eq.cursor == EQ_CURSOR_GAIN_SIGN);
    oled_draw_char(46, 21, '0' + g_tens, eq.cursor == EQ_CURSOR_GAIN_10);
    oled_draw_char(54, 21, '0' + g_ones, eq.cursor == EQ_CURSOR_GAIN_1);
    oled_draw_char(60, 21, '.',          false);
    oled_draw_char(66, 21, '0' + g_dec,  eq.cursor == EQ_CURSOR_GAIN_DEC);
    oled_draw_string(82, 21, "dB", false);

    // 4. Q Factor (y: 31..39): "Q   :  1 . 4 1" or "1 0 . 0 0"
    float q = eq.band.q_factor;
    if (q < 0.10f) q = 0.10f;
    if (q > 10.00f) q = 10.00f;
    int q_x100 = (int)roundf(q * 100.0f);
    int q_tens = q_x100 / 1000;
    int q_ones = (q_x100 / 100) % 10;
    int q_dec1 = (q_x100 / 10) % 10;
    int q_dec2 = q_x100 % 10;

    oled_draw_string(2, 31, "Q   :", false);
    oled_draw_char(40, 31, q_tens ? ('0' + q_tens) : ' ', eq.cursor == EQ_CURSOR_Q_INT && q_tens > 0);
    oled_draw_char(46, 31, '0' + q_ones, eq.cursor == EQ_CURSOR_Q_INT);
    oled_draw_char(54, 31, '.',          false);
    oled_draw_char(60, 31, '0' + q_dec1, eq.cursor == EQ_CURSOR_Q_DEC1);
    oled_draw_char(68, 31, '0' + q_dec2, eq.cursor == EQ_CURSOR_Q_DEC2);

    // 5. Hint text (y: 41..48)
    oled_draw_string(2, 42, "< >:Move  Turn:0-9", false);

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
        oled_draw_string(25, 27, "SAVED TO DAC!", false);
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
        s_frame_tick_cnt++;

        if (!s_oled_online) {
            s_probe_tick_cnt++;
            if (s_probe_tick_cnt >= 20) { // Non-blocking 2 s hot-plug probe
                s_probe_tick_cnt = 0;
                ssd1306_probe_and_init();
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (s_usb_suspended) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        xSemaphoreTake(s_ui_mutex, portMAX_DELAY);
        TickType_t last_act = s_last_activity_tick;
        uint16_t dim_sec = s_dim_sec;
        bool blanked = s_is_blanked;
        bool dimmed = s_is_dimmed;
        xSemaphoreGive(s_ui_mutex);

        if (dim_sec > 0 && !webhid_peq_is_streaming()) {
            uint32_t idle_sec = (uint32_t)(pdTICKS_TO_MS(xTaskGetTickCount() - last_act) / 1000U);
            if (!blanked && idle_sec >= OLED_BLANK_SEC) {
                s_is_blanked = true;
                ssd1306_send_cmd1(0xAE);
            } else if (!blanked && !dimmed && idle_sec >= dim_sec) {
                s_is_dimmed = true;
                ssd1306_send_cmd2(0x81, 0x1F);
            }
        }

        if (s_is_dimmed && (s_frame_tick_cnt % 100) == 0) {
            static const int8_t shifts[4] = { 0, 1, 0, -1 };
            int phase = (s_frame_tick_cnt / 100) & 3;
            s_shift_x = shifts[phase];
            s_shift_y = shifts[(phase + 1) & 3];
        }

        if (!s_is_blanked) {
            render_screen();
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

esp_err_t oled_display_init(void)
{
    ESP_LOGI(TAG, "Initializing I2C Master bus on SDA=%d, SCL=%d...",
             BOARD_I2C_SDA_PIN, BOARD_I2C_SCL_PIN);

    s_ui_mutex = xSemaphoreCreateMutex();
    if (!s_ui_mutex) return ESP_ERR_NO_MEM;

    s_last_activity_tick = xTaskGetTickCount();
    s_playback_view = (oled_playback_view_t)(peq_dsp_get_oled_view() % OLED_VIEW_COUNT);
    s_dim_sec = peq_dsp_get_oled_dim_sec();

    memset(&s_current_state, 0, sizeof(s_current_state));
    snprintf(s_current_state.mode_str, sizeof(s_current_state.mode_str), "USB DAC");
    snprintf(s_current_state.sample_rate_str, sizeof(s_current_state.sample_rate_str), "48.0k 24b");
    snprintf(s_current_state.title, sizeof(s_current_state.title), "ASCENDANCE Hi-Fi");
    snprintf(s_current_state.subtitle, sizeof(s_current_state.subtitle), "Ready");
    snprintf(s_current_state.eq_preset, sizeof(s_current_state.eq_preset), "Slot 1");
    s_current_state.volume = 50;
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

    if (ssd1306_probe_and_init() != ESP_OK) {
        ESP_LOGW(TAG, "No SSD1306 found on 0x3C/0x3D at boot; background hot-plug probe active.");
    }

    memset(&s_current_menu_state, 0, sizeof(s_current_menu_state));
    snprintf(s_current_menu_state.current_preset, sizeof(s_current_menu_state.current_preset), "Slot 1");
    snprintf(s_current_menu_state.current_source, sizeof(s_current_menu_state.current_source), "USB DAC");
    s_current_menu_state.limiter_enabled = peq_dsp_is_limiter_enabled();
    s_current_menu_state.loudness_mode = (uint8_t)peq_dsp_get_loudness_mode();
    s_current_menu_state.oled_view = (uint8_t)s_playback_view;
    peq_harmonics_cfg_t harm;
    peq_dsp_get_harmonics_cfg(&harm);
    s_current_menu_state.bass_enabled = harm.bass_enabled;
    s_current_menu_state.exciter_enabled = harm.exciter_enabled;
    s_current_menu_state.exciter_mode = (uint8_t)harm.exciter_mode;

    s_current_eq_state.active_band = 0;
    s_current_eq_state.num_bands = 5;
    s_current_eq_state.cursor = EQ_CURSOR_FREQ_1K;
    s_current_eq_state.band.type = PEQ_FILTER_PEAKING;
    s_current_eq_state.band.freq_hz = 1000.0f;
    s_current_eq_state.band.gain_db = 0.0f;
    s_current_eq_state.band.q_factor = 1.0f;
    s_current_eq_state.band.enabled = true;
    s_current_eq_state.saved_notification = false;

    xTaskCreatePinnedToCore(oled_ui_task, "oled_ui_task", 4096, NULL, 2, NULL, 0);
    return ESP_OK;
}

void oled_display_notify_activity(void)
{
    if (!s_ui_mutex) return;
    bool need_wake = false;
    bool need_bright = false;

    xSemaphoreTake(s_ui_mutex, portMAX_DELAY);
    s_last_activity_tick = xTaskGetTickCount();
    if (s_is_blanked) {
        s_is_blanked = false;
        s_is_dimmed = false;
        need_wake = true;
        need_bright = true;
    } else if (s_is_dimmed) {
        s_is_dimmed = false;
        need_bright = true;
    }
    xSemaphoreGive(s_ui_mutex);

    if (need_wake && !s_usb_suspended) ssd1306_send_cmd1(0xAF);
    if (need_bright && !s_usb_suspended) ssd1306_send_cmd2(0x81, 0xCF);
}

void oled_display_set_dim_sec(uint16_t dim_sec)
{
    if (!s_ui_mutex) return;
    xSemaphoreTake(s_ui_mutex, portMAX_DELAY);
    s_dim_sec = dim_sec;
    xSemaphoreGive(s_ui_mutex);
    oled_display_notify_activity();
}

void oled_display_sleep(void)
{
    s_usb_suspended = true;
    ssd1306_send_cmd1(0xAE);
}

void oled_display_wake(void)
{
    s_usb_suspended = false;
    s_is_blanked = false;
    s_is_dimmed = false;
    if (s_ui_mutex) {
        xSemaphoreTake(s_ui_mutex, portMAX_DELAY);
        s_last_activity_tick = xTaskGetTickCount();
        xSemaphoreGive(s_ui_mutex);
    }
    if (s_oled_online) {
        ssd1306_send_cmd2(0x81, 0xCF);
        ssd1306_send_cmd1(0xAF);
    }
}

void oled_display_set_state(const ui_display_state_t *state)
{
    if (!state || !s_ui_mutex) return;
    xSemaphoreTake(s_ui_mutex, portMAX_DELAY);
    s_current_state = *state;
    s_current_state.mode_str[sizeof(s_current_state.mode_str) - 1] = '\0';
    s_current_state.sample_rate_str[sizeof(s_current_state.sample_rate_str) - 1] = '\0';
    s_current_state.title[sizeof(s_current_state.title) - 1] = '\0';
    s_current_state.subtitle[sizeof(s_current_state.subtitle) - 1] = '\0';
    s_current_state.eq_preset[sizeof(s_current_state.eq_preset) - 1] = '\0';
    xSemaphoreGive(s_ui_mutex);
    oled_display_notify_activity();
}

void oled_display_show_volume_overlay(int volume)
{
    if (!s_ui_mutex) return;
    xSemaphoreTake(s_ui_mutex, portMAX_DELAY);
    s_overlay_vol_timeout = 15;
    s_overlay_vol_val = volume;
    s_current_state.volume = volume;
    xSemaphoreGive(s_ui_mutex);
    oled_display_notify_activity();
}

void oled_display_show_eq_overlay(const char *preset_name)
{
    if (!s_ui_mutex || !preset_name) return;
    xSemaphoreTake(s_ui_mutex, portMAX_DELAY);
    s_overlay_eq_timeout = 20;
    snprintf(s_overlay_eq_name, sizeof(s_overlay_eq_name), "%.23s", preset_name);
    s_curve_dirty = true;
    xSemaphoreGive(s_ui_mutex);
    oled_display_notify_activity();
}

void oled_display_set_eq_preset(const char *preset_name)
{
    if (!s_ui_mutex || !preset_name) return;
    xSemaphoreTake(s_ui_mutex, portMAX_DELAY);
    snprintf(s_current_state.eq_preset, sizeof(s_current_state.eq_preset), "%.15s", preset_name);
    s_curve_dirty = true;
    xSemaphoreGive(s_ui_mutex);
    oled_display_notify_activity();
}

void oled_display_set_sample_rate(int sample_rate)
{
    if (!s_ui_mutex || sample_rate <= 0) return;
    xSemaphoreTake(s_ui_mutex, portMAX_DELAY);
    snprintf(s_current_state.sample_rate_str, sizeof(s_current_state.sample_rate_str),
             "%.1fk 24b", (float)sample_rate / 1000.0f);
    s_curve_dirty = true;
    xSemaphoreGive(s_ui_mutex);
    oled_display_notify_activity();
}

void oled_display_set_ui_mode(ui_mode_t mode)
{
    if (!s_ui_mutex) return;
    xSemaphoreTake(s_ui_mutex, portMAX_DELAY);
    s_current_ui_mode = mode;
    xSemaphoreGive(s_ui_mutex);
    oled_display_notify_activity();
}

ui_mode_t oled_display_get_ui_mode(void)
{
    if (!s_ui_mutex) return UI_MODE_PLAYBACK;
    xSemaphoreTake(s_ui_mutex, portMAX_DELAY);
    ui_mode_t mode = s_current_ui_mode;
    xSemaphoreGive(s_ui_mutex);
    return mode;
}

void oled_display_update_menu_ex(const ui_menu_state_t *menu_state)
{
    if (!s_ui_mutex || !menu_state) return;
    xSemaphoreTake(s_ui_mutex, portMAX_DELAY);
    s_current_menu_state = *menu_state;
    s_current_menu_state.current_preset[sizeof(s_current_menu_state.current_preset) - 1] = '\0';
    s_current_menu_state.current_source[sizeof(s_current_menu_state.current_source) - 1] = '\0';
    xSemaphoreGive(s_ui_mutex);
    oled_display_notify_activity();
}

void oled_display_update_menu(int selected_index, const char *preset_name, const char *source_name, float preamp_gain_db)
{
    if (!s_ui_mutex) return;
    xSemaphoreTake(s_ui_mutex, portMAX_DELAY);
    s_current_menu_state.selected_idx = selected_index;
    if (preset_name) {
        snprintf(s_current_menu_state.current_preset, sizeof(s_current_menu_state.current_preset), "%.23s", preset_name);
    }
    if (source_name) {
        snprintf(s_current_menu_state.current_source, sizeof(s_current_menu_state.current_source), "%.19s", source_name);
    }
    s_current_menu_state.preamp_gain_db = preamp_gain_db;
    xSemaphoreGive(s_ui_mutex);
    oled_display_notify_activity();
}

void oled_display_update_eq_adjust(const ui_eq_adjust_state_t *eq_state)
{
    if (!s_ui_mutex || !eq_state) return;
    xSemaphoreTake(s_ui_mutex, portMAX_DELAY);
    s_current_eq_state = *eq_state;
    s_curve_dirty = true;
    xSemaphoreGive(s_ui_mutex);
    oled_display_notify_activity();
}

void oled_display_set_playback_view(oled_playback_view_t view)
{
    if ((unsigned int)view >= OLED_VIEW_COUNT) view = OLED_VIEW_HUD;
    if (!s_ui_mutex) return;
    xSemaphoreTake(s_ui_mutex, portMAX_DELAY);
    s_playback_view = view;
    if (view == OLED_VIEW_CURVE || view == OLED_VIEW_SPLIT) {
        s_curve_dirty = true;
    }
    xSemaphoreGive(s_ui_mutex);
    oled_display_notify_activity();
}

oled_playback_view_t oled_display_get_playback_view(void)
{
    if (!s_ui_mutex) return OLED_VIEW_HUD;
    xSemaphoreTake(s_ui_mutex, portMAX_DELAY);
    oled_playback_view_t v = s_playback_view;
    xSemaphoreGive(s_ui_mutex);
    return v;
}

void oled_display_invalidate_curve(void)
{
    if (!s_ui_mutex) return;
    xSemaphoreTake(s_ui_mutex, portMAX_DELAY);
    s_curve_dirty = true;
    xSemaphoreGive(s_ui_mutex);
}

