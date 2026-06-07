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
#define OLED_PROGRESS_X 8
#define OLED_PROGRESS_Y 34
#define OLED_PROGRESS_W 112
#define OLED_PROGRESS_H 14
#define OLED_ANIMATION_FRAME_MS 35
#define OLED_ANIMATION_MAX_STEPS 12

// U8G2 显示对象，以及当前已经显示到屏幕上的进度值。
static u8g2_t s_u8g2;
static float s_displayed_progress = 0.0f;

static void oled_gpio_init(void)
{
    // 这里使用软件 SPI，所以需要手动配置时钟、数据、片选等引脚。
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
    // 通过 bit-bang 方式逐位发送一个字节到 OLED。
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

    // 这是 U8G2 的底层字节发送回调，负责把待显示数据送到 SSD1306。
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

    // 这是 U8G2 的 GPIO 和延时回调，负责复位脚、片选脚和时序等待。
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

    // 先画边框，再按进度填充内部区域。
    u8g2_DrawFrame(&s_u8g2, OLED_PROGRESS_X, OLED_PROGRESS_Y, OLED_PROGRESS_W, OLED_PROGRESS_H);

    uint8_t fill_width = (uint8_t)((OLED_PROGRESS_W - 2) * progress);
    if (fill_width > 0U) {
        u8g2_DrawBox(&s_u8g2, OLED_PROGRESS_X + 1, OLED_PROGRESS_Y + 1, fill_width, OLED_PROGRESS_H - 2);
    }
}

static void draw_status_frame(const app_status_t *status, float progress)
{
    char line[32];
    int progress_percent = (int)(progress * 100.0f + 0.5f);

    // 常态页只保留时间、提醒标记和饮水进度，界面尽量简洁。
    u8g2_ClearBuffer(&s_u8g2);

    u8g2_SetFont(&s_u8g2, u8g2_font_logisoso24_tf);
    u8g2_DrawStr(&s_u8g2, 12, 24, status->time_text);

    u8g2_SetFont(&s_u8g2, u8g2_font_5x8_tf);
    if (status->remind) {
        u8g2_DrawStr(&s_u8g2, 96, 10, "DRINK");
    }

    draw_progress_bar(progress);

    u8g2_SetFont(&s_u8g2, u8g2_font_6x12_tf);
    snprintf(line, sizeof(line), "%d%%", progress_percent);
    u8g2_DrawStr(&s_u8g2, 8, 62, line);

    snprintf(line, sizeof(line), "%.0f/%.0f mL", status->total_drink_g, status->target_ml);
    u8g2_DrawStr(&s_u8g2, 42, 62, line);

    u8g2_SendBuffer(&s_u8g2);
}

void display_init(void)
{
    // _f 版本表示使用 full buffer，绘制简单但每次是整屏刷新。
    u8g2_Setup_ssd1306_128x64_noname_f(
        &s_u8g2, U8G2_R0, u8x8_byte_esp32_spi, u8x8_gpio_and_delay_esp32);
    u8g2_InitDisplay(&s_u8g2);
    u8g2_SetPowerSave(&s_u8g2, 0);
    u8g2_ClearBuffer(&s_u8g2);
    u8g2_SendBuffer(&s_u8g2);
    s_displayed_progress = 0.0f;
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
    float target_progress = 0.0f;
    float start_progress;

    if (status->target_ml > 0.0f) {
        target_progress = status->total_drink_g / status->target_ml;
    }

    if (target_progress < 0.0f) {
        target_progress = 0.0f;
    }
    if (target_progress > 1.0f) {
        target_progress = 1.0f;
    }

    start_progress = s_displayed_progress;

    // 进度增加时用几帧过渡动画，让喝水后条形增长更自然。
    if (target_progress > start_progress) {
        float delta = target_progress - start_progress;
        int steps = (int)(delta / 0.015f);

        if (steps < 4) {
            steps = 4;
        }
        if (steps > OLED_ANIMATION_MAX_STEPS) {
            steps = OLED_ANIMATION_MAX_STEPS;
        }

        for (int i = 1; i <= steps; i++) {
            float t = (float)i / (float)steps;
            float eased = 1.0f - (1.0f - t) * (1.0f - t);
            s_displayed_progress = start_progress + delta * eased;
            draw_status_frame(status, s_displayed_progress);
            vTaskDelay(pdMS_TO_TICKS(OLED_ANIMATION_FRAME_MS));
        }
    }

    s_displayed_progress = target_progress;
    draw_status_frame(status, s_displayed_progress);
}
