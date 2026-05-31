#include <math.h>
#include <stdbool.h>
#include <stdio.h>

#include "app_state.h"
#include "bemfa_notifier.h"
#include "display.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sensors.h"
#include "wifi_manager.h"
#include "web_config.h"
#include "network_settings.h"

#define HX711_PRINT_SAMPLES 5
#define HX711_CAL_WEIGHT_G 100.0f
#define WEIGHT_STABLE_DELTA_G 3.0f
#define WEIGHT_STABLE_REQUIRED_COUNT 3

static float s_target_ml = DEFAULT_TARGET_ML;
static uint32_t s_base_reminder_interval_ms = DEFAULT_REMINDER_INTERVAL_MS;
static uint32_t s_effective_reminder_interval_ms = DEFAULT_REMINDER_INTERVAL_MS;
static float s_total_drink_g = 0.0f;
static float s_last_drink_g = 0.0f;
static float s_last_stable_weight_g = 0.0f;
static float s_removed_weight_g = 0.0f;
static float s_weight_candidate_g = 0.0f;
static int s_weight_stable_count = 0;
static TickType_t s_last_drink_tick = 0;
static TickType_t s_removed_tick = 0;
static TickType_t s_returned_tick = 0;
static bool s_prev_cup_present = false;
static bool s_waiting_for_return = false;
static bool s_waiting_for_stable_return = false;
static bool s_has_stable_weight = false;
static bool s_reminder_sent = false;

static float app_get_target_ml(void)
{
    return s_target_ml;
}

static void app_set_target_ml(float value)
{
    s_target_ml = value;
}

static float app_get_total_drink_g(void)
{
    return s_total_drink_g;
}

static uint32_t app_get_reminder_interval_ms(void)
{
    return s_base_reminder_interval_ms;
}

static void app_set_reminder_interval_ms(uint32_t value)
{
    s_base_reminder_interval_ms = value;
    s_effective_reminder_interval_ms = value;
    s_reminder_sent = false;
}

static void app_reset_total_drink(void)
{
    s_total_drink_g = 0.0f;
    s_last_drink_g = 0.0f;
    s_last_drink_tick = xTaskGetTickCount();
    s_effective_reminder_interval_ms = s_base_reminder_interval_ms;
    s_reminder_sent = false;
}

static uint32_t app_calculate_next_interval_ms(float drink_g)
{
    float scale = drink_g / DRINK_REFERENCE_G;

    if (scale < REMINDER_INTERVAL_MIN_SCALE) {
        scale = REMINDER_INTERVAL_MIN_SCALE;
    }
    if (scale > REMINDER_INTERVAL_MAX_SCALE) {
        scale = REMINDER_INTERVAL_MAX_SCALE;
    }

    return (uint32_t)(s_base_reminder_interval_ms * scale);
}

static void app_update_stable_weight(float weight_g, bool cup_present)
{
    if (!cup_present) {
        s_weight_stable_count = 0;
        s_has_stable_weight = false;
        return;
    }

    if (s_weight_stable_count == 0) {
        s_weight_candidate_g = weight_g;
        s_weight_stable_count = 1;
        s_has_stable_weight = false;
        return;
    }

    if (fabsf(weight_g - s_weight_candidate_g) <= WEIGHT_STABLE_DELTA_G) {
        s_weight_candidate_g = (s_weight_candidate_g + weight_g) * 0.5f;
        s_weight_stable_count++;
    } else {
        s_weight_candidate_g = weight_g;
        s_weight_stable_count = 1;
        s_has_stable_weight = false;
    }

    if (s_weight_stable_count >= WEIGHT_STABLE_REQUIRED_COUNT) {
        s_last_stable_weight_g = s_weight_candidate_g;
        s_has_stable_weight = true;
    }
}

static void app_update_logic(float weight_g, bool cup_present)
{
    TickType_t now = xTaskGetTickCount();
    bool had_stable_weight = s_has_stable_weight;
    float stable_weight_before_change = s_last_stable_weight_g;

    if (s_prev_cup_present && !cup_present && had_stable_weight) {
        s_removed_weight_g = stable_weight_before_change;
        s_removed_tick = now;
        s_waiting_for_return = true;
        s_waiting_for_stable_return = false;
        printf("Cup removed. Weight before drink: %.2f g\n", s_removed_weight_g);
    }

    if (!s_prev_cup_present && cup_present && s_waiting_for_return) {
        s_returned_tick = now;
        s_waiting_for_return = false;
        s_waiting_for_stable_return = true;
        printf("Cup returned. Waiting for stable weight...\n");
    }

    if (cup_present && s_waiting_for_stable_return) {
        TickType_t stable_ms = pdTICKS_TO_MS(now - s_returned_tick);
        if (stable_ms >= CUP_SETTLE_TIME_MS && s_removed_weight_g > CUP_PRESENT_MIN_G && s_has_stable_weight) {
            float returned_weight_g = s_last_stable_weight_g;
            float drink_g = s_removed_weight_g - returned_weight_g;

            if (drink_g >= DRINK_VALID_MIN_G) {
                s_last_drink_g = drink_g;
                s_total_drink_g += drink_g;
                s_last_drink_tick = now;
                s_effective_reminder_interval_ms = app_calculate_next_interval_ms(drink_g);
                s_reminder_sent = false;
                printf("Drink event: before=%.2f g after=%.2f g drank=%.2f g total=%.2f g next=%lu ms\n",
                       s_removed_weight_g, returned_weight_g, s_last_drink_g, s_total_drink_g,
                       (unsigned long)s_effective_reminder_interval_ms);
            } else {
                printf("Cup returned. Stable delta %.2f g ignored.\n", drink_g);
            }

            s_waiting_for_stable_return = false;
        }
    }

    app_update_stable_weight(weight_g, cup_present);
    s_prev_cup_present = cup_present;
}

void app_main(void)
{
    app_status_t status = {0};

    ESP_ERROR_CHECK(nvs_flash_init());

    display_init();
    display_show_boot("Smart Coaster", "Init sensors...");

    sensors_init();
    vTaskDelay(pdMS_TO_TICKS(2000));
    sensors_tare();

    web_config_init(app_get_target_ml, app_set_target_ml, app_get_total_drink_g,
                    app_get_reminder_interval_ms, app_set_reminder_interval_ms,
                    app_reset_total_drink);

    s_target_ml = web_config_load_target_ml();
    s_base_reminder_interval_ms = web_config_load_reminder_interval_ms();
    s_effective_reminder_interval_ms = s_base_reminder_interval_ms;
    s_last_drink_tick = xTaskGetTickCount();

    display_show_boot("Connect WiFi...", "LAN + Bemfa");
    wifi_manager_init_sta(WIFI_STA_SSID, WIFI_STA_PASSWORD);
    web_config_start_server();

    printf("Smart coaster started.\n");
    printf("Target drink amount: %.0f mL\n", s_target_ml);
    printf("Base reminder interval: %lu ms\n", (unsigned long)s_base_reminder_interval_ms);
    printf("HX711 offset after tare: %ld\n", (long)sensors_get_offset());
    printf("HX711 scale in code: %.3f counts/gram\n", HX711_COUNTS_PER_GRAM);
    printf("Calibration steps:\n");
    printf("1. Keep the scale empty during tare.\n");
    printf("2. Put a known weight on the scale, e.g. %.0f g.\n", HX711_CAL_WEIGHT_G);
    printf("3. Read serial 'Raw' value.\n");
    printf("4. counts_per_gram = (raw - offset) / known_grams\n");

    while (1) {
        char ip_text[32];
        int32_t raw = sensors_get_raw_average(HX711_PRINT_SAMPLES);
        float weight_g = (raw - sensors_get_offset()) / HX711_COUNTS_PER_GRAM;
        bool cup_present = sensors_has_cup();

        if (weight_g < 0.0f) {
            weight_g = 0.0f;
        }

        app_update_logic(weight_g, cup_present);

        TickType_t now = xTaskGetTickCount();
        uint32_t elapsed_ms = pdTICKS_TO_MS(now - s_last_drink_tick);

        status.cup_present = cup_present;
        status.weight_g = weight_g;
        status.last_drink_g = s_last_drink_g;
        status.total_drink_g = s_total_drink_g;
        status.target_ml = s_target_ml;
        status.reminder_interval_ms = s_effective_reminder_interval_ms;
        status.remind = elapsed_ms >= s_effective_reminder_interval_ms;
        status.next_reminder_s = status.remind ? 0U : (s_effective_reminder_interval_ms - elapsed_ms) / 1000U;

        wifi_manager_get_ip_string(ip_text, sizeof(ip_text));

        if (status.remind && !s_reminder_sent && wifi_manager_is_connected()) {
            unsigned int interval_minutes = s_effective_reminder_interval_ms / 60000U;
            if (bemfa_notifier_send_reminder(status.total_drink_g, status.target_ml, interval_minutes)) {
                s_reminder_sent = true;
                printf("Reminder push sent.\n");
            }
        }

        printf("Raw:%ld Offset:%ld Weight:%.2f g Cup:%d Last:%.2f g Total:%.2f g Next:%lu s LAN:http://%s/\n",
               (long)raw,
               (long)sensors_get_offset(),
               status.weight_g,
               status.cup_present ? 1 : 0,
               status.last_drink_g,
               status.total_drink_g,
               (unsigned long)status.next_reminder_s,
               ip_text);

        display_show_status(&status);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
