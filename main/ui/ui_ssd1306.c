#include "ui_ssd1306.h"
#include "board_pins.h"

#include <string.h>
#include "esp_log.h"
#include "driver/i2c_master.h"

#define TAG "SSD1306"
#define OLED_I2C_TIMEOUT_MS 10

static i2c_master_bus_handle_t s_i2c_bus = NULL;
static i2c_master_dev_handle_t s_oled_dev = NULL;
static uint8_t s_oled_addr = I2C_OLED_ADDR_DEFAULT;
static bool s_oled_online = false;
static uint8_t s_framebuffer[OLED_BUFFER_SIZE];
static int s_shift_x = 0;
static int s_shift_y = 0;

bool ssd1306_is_online(void)
{
    return s_oled_online;
}

void ssd1306_set_pixel_shift(int shift_x, int shift_y)
{
    s_shift_x = shift_x;
    s_shift_y = shift_y;
}

esp_err_t ssd1306_bus_init(void)
{
    ESP_LOGI(TAG, "Initializing I2C Master bus on SDA=%d, SCL=%d...",
             BOARD_I2C_SDA_PIN, BOARD_I2C_SCL_PIN);
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
    }
    return ret;
}

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

void oled_clear(void)
{
    memset(s_framebuffer, 0, sizeof(s_framebuffer));
}

void oled_draw_pixel(int x, int y, bool on)
{
    int sx = x + s_shift_x;
    int sy = y + s_shift_y;
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

void oled_draw_char(int x, int y, char c, bool invert)
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

void oled_draw_string(int x, int y, const char *str, bool invert)
{
    if (!str) return;
    int cx = x;
    while (*str && cx < OLED_WIDTH - 5) {
        oled_draw_char(cx, y, *str, invert);
        cx += 6;
        str++;
    }
}

void oled_draw_hline(int x, int y, int w, bool on)
{
    for (int i = 0; i < w; i++) {
        oled_draw_pixel(x + i, y, on);
    }
}

void oled_draw_vline(int x, int y, int h, bool on)
{
    for (int i = 0; i < h; i++) {
        oled_draw_pixel(x, y + i, on);
    }
}

void oled_draw_rect(int x, int y, int w, int h, bool fill, bool on)
{
    for (int r = 0; r < h; r++) {
        for (int c = 0; c < w; c++) {
            if (fill || r == 0 || r == h - 1 || c == 0 || c == w - 1) {
                oled_draw_pixel(x + c, y + r, on);
            }
        }
    }
}

void oled_draw_progress_bar(int x, int y, int w, int h, int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    oled_draw_rect(x, y, w, h, false, true);
    int fill_w = ((w - 4) * percent) / 100;
    if (fill_w > 0) {
        oled_draw_rect(x + 2, y + 2, fill_w, h - 4, true, true);
    }
}

esp_err_t ssd1306_send_cmd2(uint8_t c1, uint8_t c2)
{
    if (!s_oled_dev || !s_oled_online) return ESP_ERR_INVALID_STATE;
    const uint8_t cmds[3] = {0x00, c1, c2};
    return i2c_master_transmit(s_oled_dev, cmds, sizeof(cmds), OLED_I2C_TIMEOUT_MS);
}

esp_err_t ssd1306_send_cmd1(uint8_t c1)
{
    if (!s_oled_dev || !s_oled_online) return ESP_ERR_INVALID_STATE;
    const uint8_t cmds[2] = {0x00, c1};
    return i2c_master_transmit(s_oled_dev, cmds, sizeof(cmds), OLED_I2C_TIMEOUT_MS);
}

esp_err_t ssd1306_probe_and_init(void)
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
        s_shift_x = 0;
        s_shift_y = 0;
    } else {
        s_oled_online = false;
    }
    return ret;
}

esp_err_t oled_flush(void)
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

