#include "display.h"

#include <stdint.h>
#include <stdio.h>

#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "u8g2.h"

#define OLED_SCK GPIO_NUM_14
#define OLED_MOSI GPIO_NUM_13
#define OLED_RES GPIO_NUM_27
#define OLED_DC GPIO_NUM_26
#define OLED_CS GPIO_NUM_25

#define OLED_WIDTH 128
#define OLED_PROGRESS_Y 52
#define OLED_PROGRESS_H 10

static u8g2_t s_u8g2;

static void oled_gpio_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << OLED_SCK) | (1ULL << OLED_MOSI) |
                        (1ULL << OLED_RES) | (1ULL << OLED_DC) | (1ULL << OLED_CS),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    gpio_set_level(OLED_CS, 1);
    gpio_set_level(OLED_SCK, 0);
    gpio_set_level(OLED_MOSI, 0);
    gpio_set_level(OLED_DC, 0);
    gpio_set_level(OLED_RES, 1);
}

static void oled_spi_write_byte(uint8_t value)
{
    for (int bit = 0; bit < 8; bit++) {
        gpio_set_level(OLED_SCK, 0);
        gpio_set_level(OLED_MOSI, (value & 0x80U) != 0U);
        esp_rom_delay_us(1);
        gpio_set_level(OLED_SCK, 1);
        esp_rom_delay_us(1);
        value <<= 1;
    }
}

static uint8_t u8x8_byte_esp32_spi(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr)
{
    (void)u8x8;

    switch (msg) {
    case U8X8_MSG_BYTE_INIT:
        return 1;
    case U8X8_MSG_BYTE_SET_DC:
        gpio_set_level(OLED_DC, arg_int);
        return 1;
    case U8X8_MSG_BYTE_START_TRANSFER:
        gpio_set_level(OLED_CS, 0);
        return 1;
    case U8X8_MSG_BYTE_SEND: {
        uint8_t *data = (uint8_t *)arg_ptr;
        while (arg_int > 0U) {
            oled_spi_write_byte(*data++);
            arg_int--;
        }
        return 1;
    }
    case U8X8_MSG_BYTE_END_TRANSFER:
        gpio_set_level(OLED_CS, 1);
        return 1;
    default:
        return 0;
    }
}

static uint8_t u8x8_gpio_and_delay_esp32(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr)
{
    (void)u8x8;
    (void)arg_ptr;

    switch (msg) {
    case U8X8_MSG_GPIO_AND_DELAY_INIT:
        oled_gpio_init();
        return 1;
    case U8X8_MSG_DELAY_MILLI:
        vTaskDelay(pdMS_TO_TICKS(arg_int));
        return 1;
    case U8X8_MSG_DELAY_10MICRO:
        esp_rom_delay_us(10);
        return 1;
    case U8X8_MSG_DELAY_100NANO:
        return 1;
    case U8X8_MSG_GPIO_DC:
        gpio_set_level(OLED_DC, arg_int);
        return 1;
    case U8X8_MSG_GPIO_CS:
        gpio_set_level(OLED_CS, arg_int);
        return 1;
    case U8X8_MSG_GPIO_RESET:
        gpio_set_level(OLED_RES, arg_int);
        return 1;
    case U8X8_MSG_GPIO_SPI_CLOCK:
        gpio_set_level(OLED_SCK, arg_int);
        return 1;
    case U8X8_MSG_GPIO_SPI_DATA:
        gpio_set_level(OLED_MOSI, arg_int);
        return 1;
    default:
        return 1;
    }
}

static void draw_progress_bar(float progress)
{
    if (progress < 0.0f) {
        progress = 0.0f;
    }
    if (progress > 1.0f) {
        progress = 1.0f;
    }

    u8g2_DrawFrame(&s_u8g2, 0, OLED_PROGRESS_Y, OLED_WIDTH, OLED_PROGRESS_H);

    uint8_t fill_width = (uint8_t)((OLED_WIDTH - 2) * progress);
    if (fill_width > 0U) {
        u8g2_DrawBox(&s_u8g2, 1, OLED_PROGRESS_Y + 1, fill_width, OLED_PROGRESS_H - 2);
    }
}

void display_init(void)
{
    u8g2_Setup_ssd1306_128x64_noname_f(
        &s_u8g2, U8G2_R0, u8x8_byte_esp32_spi, u8x8_gpio_and_delay_esp32);
    u8g2_InitDisplay(&s_u8g2);
    u8g2_SetPowerSave(&s_u8g2, 0);
    u8g2_ClearBuffer(&s_u8g2);
    u8g2_SendBuffer(&s_u8g2);
}

void display_show_boot(const char *line1, const char *line2)
{
    u8g2_ClearBuffer(&s_u8g2);
    u8g2_SetFont(&s_u8g2, u8g2_font_6x12_tf);
    u8g2_DrawStr(&s_u8g2, 0, 18, line1);
    u8g2_SetFont(&s_u8g2, u8g2_font_5x8_tf);
    u8g2_DrawStr(&s_u8g2, 0, 34, line2);
    u8g2_SendBuffer(&s_u8g2);
}

void display_show_status(const app_status_t *status)
{
    char line[32];
    float progress = 0.0f;

    if (status->target_ml > 0.0f) {
        progress = status->total_drink_g / status->target_ml;
    }

    u8g2_ClearBuffer(&s_u8g2);

    u8g2_SetFont(&s_u8g2, u8g2_font_6x12_tf);
    u8g2_DrawStr(&s_u8g2, 0, 10, status->remind ? "DRINK WATER!" : "SMART COASTER");

    u8g2_SetFont(&s_u8g2, u8g2_font_5x8_tf);

    snprintf(line, sizeof(line), "Cup:%s W:%5.0fg", status->cup_present ? "ON" : "OFF", status->weight_g);
    u8g2_DrawStr(&s_u8g2, 0, 22, line);

    snprintf(line, sizeof(line), "Last:%5.0fg", status->last_drink_g);
    u8g2_DrawStr(&s_u8g2, 0, 32, line);

    snprintf(line, sizeof(line), "Tot:%4.0fg T:%4.0f", status->total_drink_g, status->target_ml);
    u8g2_DrawStr(&s_u8g2, 0, 42, line);

    snprintf(line, sizeof(line), "%3lus", (unsigned long)status->next_reminder_s);
    u8g2_DrawStr(&s_u8g2, 92, 32, line);

    draw_progress_bar(progress);
    u8g2_SendBuffer(&s_u8g2);
}
