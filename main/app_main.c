#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "app_state.h"
#include "bemfa_notifier.h"
#include "display.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "network_settings.h"
#include "nvs_flash.h"
#include "sensors.h"
#include "web_config.h"
#include "wifi_manager.h"

#define HX711_PRINT_SAMPLES 5
#define HX711_CAL_WEIGHT_G 100.0f
#define WEIGHT_STABLE_DELTA_G 3.0f
#define WEIGHT_STABLE_REQUIRED_COUNT 3
#define WIFI_CONNECT_RETRY_MAX 5
#define WIFI_CONNECT_TIMEOUT_MS 10000
#define WIFI_AP_SSID "DrinkCoaster-Setup"
#define WIFI_AP_PASSWORD "12345678"

// 这些全局变量用于保存系统运行过程中的长期状态，
// 包括饮水进度、提醒计时、平稳重量判断以及 WiFi 配置等。
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
static char s_wifi_ssid[33];
static char s_wifi_password[65];

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
    // 根据本次饮水量，动态调整下一次提醒的时间间隔。
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
    // 只有杯子在杯垫上时，平稳重量才有意义。
    if (!cup_present) {
        s_weight_stable_count = 0;
        s_has_stable_weight = false;
        return;
    }

    // 第一次采样得到的重量，先作为“候选稳定重量”。
    if (s_weight_stable_count == 0) {
        s_weight_candidate_g = weight_g;
        s_weight_stable_count = 1;
        s_has_stable_weight = false;
        return;
    }

    // 如果连续读数都在允许波动范围内，
    // 就认为它们属于同一个稳定区间，并对候选值做简单平滑。
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

    // 当检测到杯子被拿起时，记录拿起前最后一次稳定重量，
    // 这个值作为“喝水前重量”。
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

    // 只有在杯子放回后再次稳定，才真正计算本次饮水量。
    if (cup_present && s_waiting_for_stable_return) {
        TickType_t stable_ms = pdTICKS_TO_MS(now - s_returned_tick);
        if (stable_ms >= CUP_SETTLE_TIME_MS && s_removed_weight_g > CUP_PRESENT_MIN_G && s_has_stable_weight) {
            float returned_weight_g = s_last_stable_weight_g;
            float drink_g = s_removed_weight_g - returned_weight_g;

            if (drink_g >= DRINK_VALID_MIN_G) {
                s_last_drink_g = drink_g;
                s_total_drink_g += drink_g;
                s_last_drink_tick = now;
                // 喝得多，下次提醒适当延后；喝得少，下次提醒适当提前。
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

static void app_load_wifi_credentials(void)
{
    // 优先使用网页保存过的 WiFi 配置，
    // 如果没有保存过，就退回到 network_settings.h 中的默认配置。
    if (!web_config_load_wifi(s_wifi_ssid, sizeof(s_wifi_ssid), s_wifi_password, sizeof(s_wifi_password))) {
        snprintf(s_wifi_ssid, sizeof(s_wifi_ssid), "%s", WIFI_STA_SSID);
        snprintf(s_wifi_password, sizeof(s_wifi_password), "%s", WIFI_STA_PASSWORD);
    }
}

static bool app_try_connect_wifi(void)
{
    for (int attempt = 1; attempt <= WIFI_CONNECT_RETRY_MAX; attempt++) {
        char line[32];

        // 当前逻辑下，只有检测到杯子在位时才尝试恢复联网。
        if (!sensors_has_cup()) {
            return false;
        }

        snprintf(line, sizeof(line), "Try %d/%d", attempt, WIFI_CONNECT_RETRY_MAX);
        display_show_boot("Connecting WiFi", line);

        if (wifi_manager_connect_sta(s_wifi_ssid, s_wifi_password, WIFI_CONNECT_TIMEOUT_MS)) {
            // 对时不是主功能的硬性前提，所以这里只尝试同步时间，
            // 即使失败也不阻塞后续业务运行。
            display_show_boot("WiFi connected", "Sync time...");
            wifi_manager_sync_time(WIFI_CONNECT_TIMEOUT_MS);
            web_config_start_server(false);
            return true;
        }
    }

    return false;
}

static bool app_run_ap_config_mode(void)
{
    char new_ssid[33];
    char new_password[65];

    // 开启 SoftAP 配网，并持续显示热点 IP，
    // 直到用户通过网页提交新的 WiFi 配置。
    wifi_manager_start_ap(WIFI_AP_SSID, WIFI_AP_PASSWORD);
    web_config_start_server(true);

    while (1) {
        display_show_boot("AP config mode", "192.168.4.1");

        if (web_config_consume_wifi_update(new_ssid, sizeof(new_ssid), new_password, sizeof(new_password))) {
            snprintf(s_wifi_ssid, sizeof(s_wifi_ssid), "%s", new_ssid);
            snprintf(s_wifi_password, sizeof(s_wifi_password), "%s", new_password);
            web_config_stop_server();
            wifi_manager_stop();
            display_show_boot("Config saved", "Reconnect WiFi");
            vTaskDelay(pdMS_TO_TICKS(800));
            return true;
        }

        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

static void app_ensure_network_ready(void)
{
    // 先尝试以 STA 模式连接路由器；
    // 如果连续失败，再进入 AP 配网模式。
    while (1) {
        if (!sensors_has_cup()) {
            return;
        }

        if (app_try_connect_wifi()) {
            return;
        }

        if (!app_run_ap_config_mode()) {
            continue;
        }
    }
}

static void app_fill_status(app_status_t *status, float weight_g, bool cup_present)
{
    TickType_t now = xTaskGetTickCount();
    uint32_t elapsed_ms = pdTICKS_TO_MS(now - s_last_drink_tick);

    // 统一整理一份当前状态快照，
    // 让 OLED 显示和串口打印使用同一组数据。
    memset(status, 0, sizeof(*status));

    status->cup_present = cup_present;
    status->wifi_connected = wifi_manager_is_connected();
    status->wifi_config_mode = wifi_manager_is_ap_mode();
    status->time_synced = wifi_manager_has_valid_time();
    status->weight_g = weight_g;
    status->last_drink_g = s_last_drink_g;
    status->total_drink_g = s_total_drink_g;
    status->target_ml = s_target_ml;
    status->reminder_interval_ms = s_effective_reminder_interval_ms;
    status->remind = elapsed_ms >= s_effective_reminder_interval_ms;
    status->next_reminder_s = status->remind ? 0U : (s_effective_reminder_interval_ms - elapsed_ms) / 1000U;

    if (status->wifi_config_mode) {
        snprintf(status->wifi_text, sizeof(status->wifi_text), "WiFi: AP config");
    } else if (status->wifi_connected) {
        snprintf(status->wifi_text, sizeof(status->wifi_text), "WiFi: connected");
    } else {
        snprintf(status->wifi_text, sizeof(status->wifi_text), "WiFi: offline");
    }

    wifi_manager_get_ip_string(status->ip_text, sizeof(status->ip_text));
    wifi_manager_get_time_string(status->time_text, sizeof(status->time_text));
}

void app_main(void)
{
    app_status_t status = {0};

    ESP_ERROR_CHECK(nvs_flash_init());

    // 基本硬件与界面初始化。
    display_init();
    display_show_boot("Smart Coaster", "Init sensors...");

    sensors_init();
    wifi_manager_init();
    vTaskDelay(pdMS_TO_TICKS(2000));
    sensors_tare();

    // 注册网页配置回调，并读取已经保存在 NVS 中的目标饮水量和提醒间隔。
    web_config_init(app_get_target_ml, app_set_target_ml, app_get_total_drink_g,
                    app_get_reminder_interval_ms, app_set_reminder_interval_ms,
                    app_reset_total_drink);

    s_target_ml = web_config_load_target_ml();
    s_base_reminder_interval_ms = web_config_load_reminder_interval_ms();
    s_effective_reminder_interval_ms = s_base_reminder_interval_ms;
    s_last_drink_tick = xTaskGetTickCount();

    app_load_wifi_credentials();
    if (sensors_has_cup()) {
        app_ensure_network_ready();
    }

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
        // 每一轮循环都重新读取当前平均重量和杯子在位状态。
        int32_t raw = sensors_get_raw_average(HX711_PRINT_SAMPLES);
        float weight_g = (raw - sensors_get_offset()) / HX711_COUNTS_PER_GRAM;
        bool cup_present = sensors_has_cup();

        if (weight_g < 0.0f) {
            weight_g = 0.0f;
        }

        if (cup_present && !wifi_manager_is_connected()) {
            web_config_stop_server();
            app_ensure_network_ready();
            continue;
        }

        // 如果 AP 配网页提交了新的 WiFi 配置，
        // 就停止当前网络服务，并使用新配置重新联网。
        if (web_config_consume_wifi_update(s_wifi_ssid, sizeof(s_wifi_ssid),
                                           s_wifi_password, sizeof(s_wifi_password))) {
            web_config_stop_server();
            wifi_manager_stop();
            app_ensure_network_ready();
            continue;
        }

        app_update_logic(weight_g, cup_present);
        app_fill_status(&status, weight_g, cup_present);

        // 每个提醒周期内只推送一次，并且只有联网成功时才发送微信提醒。
        if (status.remind && !s_reminder_sent && wifi_manager_is_connected()) {
            unsigned int interval_minutes = s_effective_reminder_interval_ms / 60000U;
            if (bemfa_notifier_send_reminder(status.total_drink_g, status.target_ml, interval_minutes)) {
                s_reminder_sent = true;
                printf("Reminder push sent.\n");
            }
        }

        printf("Raw:%ld Offset:%ld Weight:%.2f g Cup:%d Last:%.2f g Total:%.2f g Next:%lu s IP:%s Time:%s\n",
               (long)raw,
               (long)sensors_get_offset(),
               status.weight_g,
               status.cup_present ? 1 : 0,
               status.last_drink_g,
               status.total_drink_g,
               (unsigned long)status.next_reminder_s,
               status.ip_text,
               status.time_text);

        // OLED 常态页面只显示时间和饮水进度。
        display_show_status(&status);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
