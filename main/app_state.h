#pragma once

#include <stdbool.h>
#include <stdint.h>

// 下面这些宏决定了称重换算、稳定判断和提醒策略的核心参数。
#define HX711_COUNTS_PER_GRAM 421.84f
#define CUP_PRESENT_MIN_G 30.0f
#define DRINK_VALID_MIN_G 5.0f
#define DEFAULT_REMINDER_INTERVAL_MS (1 * 60 * 1000)
#define CUP_SETTLE_TIME_MS 1200
#define DEFAULT_TARGET_ML 2000.0f
#define DRINK_REFERENCE_G 120.0f
#define REMINDER_INTERVAL_MIN_SCALE 0.5f
#define REMINDER_INTERVAL_MAX_SCALE 2.0f

// 这个结构体用于收集当前一帧要显示/打印的运行状态。
typedef struct {
    bool cup_present;
    bool remind;
    bool wifi_connected;
    bool wifi_config_mode;
    bool time_synced;
    float weight_g;
    float last_drink_g;
    float total_drink_g;
    float target_ml;
    uint32_t reminder_interval_ms;
    uint32_t next_reminder_s;
    char wifi_text[24];
    char ip_text[32];
    char time_text[16];
} app_status_t;
