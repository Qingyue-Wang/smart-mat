#include "sensors.h"
#include "app_state.h"

#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define HX711_DOUT GPIO_NUM_4
#define HX711_SCK  GPIO_NUM_5

#define TCRT5000_DO GPIO_NUM_18
#define TCRT5000_ACTIVE_LEVEL 0

static int32_t s_hx711_offset = 0;

static bool hx711_is_ready(void)
{
    return gpio_get_level(HX711_DOUT) == 0;
}

static int32_t hx711_read_raw(void)
{
    int32_t data = 0;

    while (!hx711_is_ready()) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    for (int i = 0; i < 24; i++) {
        gpio_set_level(HX711_SCK, 1);
        esp_rom_delay_us(1);
        data = (data << 1) | gpio_get_level(HX711_DOUT);
        gpio_set_level(HX711_SCK, 0);
        esp_rom_delay_us(1);
    }

    gpio_set_level(HX711_SCK, 1);
    esp_rom_delay_us(1);
    gpio_set_level(HX711_SCK, 0);
    esp_rom_delay_us(1);

    if (data & 0x800000) {
        data |= ~0xFFFFFF;
    }

    return data;
}

static int32_t hx711_read_average(int samples)
{
    int64_t sum = 0;

    for (int i = 0; i < samples; i++) {
        sum += hx711_read_raw();
    }

    return (int32_t)(sum / samples);
}

void sensors_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << HX711_DOUT) | (1ULL << TCRT5000_DO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    io_conf.pin_bit_mask = (1ULL << HX711_SCK);
    io_conf.mode = GPIO_MODE_OUTPUT;
    gpio_config(&io_conf);

    gpio_set_level(HX711_SCK, 0);
}

void sensors_tare(void)
{
    s_hx711_offset = hx711_read_average(10);
}

int32_t sensors_get_raw_average(int samples)
{
    return hx711_read_average(samples);
}

int32_t sensors_get_offset(void)
{
    return s_hx711_offset;
}

float sensors_get_weight_grams(int samples)
{
    int32_t raw = hx711_read_average(samples);
    return (raw - s_hx711_offset) / HX711_COUNTS_PER_GRAM;
}

bool sensors_has_cup(void)
{
    return gpio_get_level(TCRT5000_DO) == TCRT5000_ACTIVE_LEVEL;
}
