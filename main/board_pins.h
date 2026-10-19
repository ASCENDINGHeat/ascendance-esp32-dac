#pragma once

#include "driver/gpio.h"

// =============================================================================
// YD-ESP32-S3 (YD-ESP32-23) Master Pinout Reference
// Board Reference: https://github.com/rtek1000/YD-ESP32-23 (Schematic V1.4)
// Schematics:
//   - docs/schematics/DSP+DAC.svg (Rev 1.3, Standalone Headless Streamer / 3 Presets)
//   - docs/schematics/DSP+DAC+SCREEN.svg (Rev 1.1, Streamer w/ Screen, Rotary & Dual Buttons, No SD)
//   - docs/schematics/DSP+DAC+LOCAL.svg (Rev 1.2, Complete DAP w/ Screen, Rotary & SD)
// =============================================================================

// --- Primary I2S Audio Output (Master to PCM5102A DAC) ---
#define BOARD_I2S_WS_PIN         GPIO_NUM_4   // Word Select / LCK (Left Header Pin 4)
#define BOARD_I2S_DATA_OUT_PIN   GPIO_NUM_5   // Serial Audio Data / DIN (Left Header Pin 5)
#define BOARD_I2S_BCK_PIN        GPIO_NUM_6   // Continuous Bit Clock / BCK (Left Header Pin 6)
#define BOARD_DAC_MUTE_PIN       GPIO_NUM_7   // Soft Mute XMT (Left Header Pin 7, HIGH = Play, LOW = Mute)

// --- Headphone Output Jacks (Physical Connectors) ---
// J1 (DSP-DAC Rev 1.3), J3 (DSP-DAC-SCREEN Rev 1.1), J2 / J1 (DSP-DAC-LOCAL Rev 1.2):
// 3.5mm TRS Stereo Jacks (DC_TRS_PADS) connected to TDA1308 outputs (L_OUT, G_OUT / AGND, R_OUT)

// --- I2C Monochrome Display (SSD1306 / SH1106 128x64 OLED) ---
#define BOARD_I2C_SDA_PIN        GPIO_NUM_8   // I2C Data Line (Left Header Pin 12)
#define BOARD_I2C_SCL_PIN        GPIO_NUM_9   // I2C Clock Line (Left Header Pin 15)
// NOTE: Standard 4-pin I2C OLED modules (OLED1 / OLED2) ground SA0 internally, setting address to 0x3C.
// If using a legacy 7-pin SPI OLED converted to I2C with DC tied to 3.3V, SA0 is HIGH (0x3D).
#define I2C_OLED_ADDR_DEFAULT    0x3C         // Standard 4-pin I2C OLED (OLED1 / OLED2 @ 0x3C)
#define I2C_OLED_ADDR_SA0_LOW    0x3C         // SA0 tied to GND
#define I2C_OLED_ADDR_SA0_HIGH   0x3D         // SA0 tied to 3.3V (7-pin SPI modules)
#define I2C_OLED_ADDR_ALT        0x3D         // Alternative I2C address fallback

// --- Rotary Encoder & User Inputs (EC11 Quadrature & Dual Buttons) ---
// Note: Hardware 10 kΩ pull-ups on GPIO 15, 16, 17, 18, 10 to 3.3V (R9..R11, R8, R12 in Rev 1.1; R5..R7 in Rev 1.2)
#define BOARD_ENC_A_PIN          GPIO_NUM_15  // Phase A (Left Header Pin 8, PCNT filtered, 10k pull-up)
#define BOARD_ENC_B_PIN          GPIO_NUM_16  // Phase B (Left Header Pin 9, PCNT filtered, 10k pull-up)
#define BOARD_ENC_SW_PIN         GPIO_NUM_17  // Integrated Push Switch (Left Header Pin 10, Active-Low, 10k pull-up)
#define BOARD_BTN_MODE_PIN       GPIO_NUM_18  // Dedicated Mode Button (Left Header Pin 11, SW6/SW4/SW1, 10k pull-up)
#define BOARD_BTN_1_PIN          GPIO_NUM_18  // Navigation Button 1 / Left (Left Header Pin 11, SW6, 10k pull-up R8)
#define BOARD_BTN_NAV_LEFT_PIN   GPIO_NUM_18  // Alias for Button 1 (◄ / Menu / Prev)
#define BOARD_BTN_2_PIN          GPIO_NUM_10  // Navigation Button 2 / Right (Left Header Pin 16, SW8, 10k pull-up R12)
#define BOARD_BTN_NAV_RIGHT_PIN  GPIO_NUM_10  // Alias for Button 2 (► / Next / Preset)

// --- Alternative 3-Button Preset Controller (Schematic 1, Rev 1.3: DSP+DAC.svg) ---
// When EC11 Rotary Encoder is omitted, GPIO 15, 16, 17 are used for instant preset switching:
#define BOARD_BTN_PRESET_1_PIN   GPIO_NUM_15  // SW1 / SW3: Preset 1 (e.g., Reference Flat / Web Stream 1, 10k pull-up)
#define BOARD_BTN_PRESET_2_PIN   GPIO_NUM_16  // SW2 / SW5: Preset 2 (e.g., Bass Boost / Web Stream 2, 10k pull-up)
#define BOARD_BTN_PRESET_3_PIN   GPIO_NUM_17  // SW3 / SW4: Preset 3 (e.g., AutoEQ Harman / Web Stream 3, 10k pull-up)

// --- MicroSD Storage (SDMMC Host Slot 1) ---
// NOTE: Omitted in DSP+DAC+SCREEN.svg (Rev 1.1), leaving GPIO 13, 14, 21 free for user expansions!
#define BOARD_SDMMC_CLK_PIN      GPIO_NUM_14  // SD Clock Line (Left Header Pin 20)
#define BOARD_SDMMC_CMD_PIN      GPIO_NUM_13  // SD Command Line (Left Header Pin 19, 10k pull-up required)
#define BOARD_SDMMC_D0_PIN       GPIO_NUM_21  // SD Data 0 Line (Right Header Pin 18, 10k pull-up required)
#define BOARD_SDMMC_D1_PIN       GPIO_NUM_47  // SD Data 1 (Right Header Pin 17, 4-bit mode)
#define BOARD_SDMMC_D2_PIN       GPIO_NUM_38  // SD Data 2 (Right Header Pin 10, 4-bit mode)
#define BOARD_SDMMC_D3_PIN       GPIO_NUM_39  // SD Data 3 (Right Header Pin 9, 4-bit mode)
#define BOARD_SDMMC_CD_PIN       GPIO_NUM_40  // Optional Card Detect Switch (Right Header Pin 8)

// --- Native USB OTG Port (USB Audio Class 1.0) ---
#define BOARD_USB_DM_PIN         GPIO_NUM_19  // USB D- (Right Header Pin 20 / Left Type-C Receptacle)
#define BOARD_USB_DP_PIN         GPIO_NUM_20  // USB D+ (Right Header Pin 19 / Left Type-C Receptacle)

// --- UART Console & Flashing Port (WCH CH343P Bridge) ---
#define BOARD_UART0_TX_PIN       GPIO_NUM_43  // Console TX (Right Header Pin 2 / Right Type-C Receptacle)
#define BOARD_UART0_RX_PIN       GPIO_NUM_44  // Console RX (Right Header Pin 3 / Right Type-C Receptacle)

// --- Onboard Peripherals ---
#define BOARD_BOOT_BTN_PIN       GPIO_NUM_0   // Onboard BOOT Button (Right Header Pin 14)
#define BOARD_RGB_LED_PIN        GPIO_NUM_48  // WS2812B RGB LED (Right Header Pin 16 / RMT driver)
#define BOARD_WS2812_PIN         GPIO_NUM_48  // Alias for onboard WS2812B RGB LED

// --- Inter-Chip Dual-MCU Bus (Optional Upgrade Link) ---
#define BOARD_INTER_I2S_WS_PIN   GPIO_NUM_1   // Inter-Chip I2S Word Select RX (Right Header Pin 4)
#define BOARD_INTER_I2S_DIN_PIN  GPIO_NUM_2   // Inter-Chip I2S Data RX (Right Header Pin 5)
#define BOARD_INTER_I2S_BCK_PIN  GPIO_NUM_3   // Inter-Chip I2S Bit Clock RX (Left Header Pin 13)
#define BOARD_INTER_UART_RX_PIN  GPIO_NUM_11  // UART1 RX from Hub (Left Header Pin 17)
#define BOARD_INTER_UART_TX_PIN  GPIO_NUM_12  // UART1 TX to Hub (Left Header Pin 18)
