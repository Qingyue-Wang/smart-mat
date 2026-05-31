#pragma once

#include "app_state.h"

typedef float (*web_getter_t)(void);
typedef void (*web_setter_t)(float value);
typedef uint32_t (*web_u32_getter_t)(void);
typedef void (*web_u32_setter_t)(uint32_t value);
typedef void (*web_action_t)(void);

void web_config_init(web_getter_t get_target_ml,
                     web_setter_t set_target_ml,
                     web_getter_t get_total_drink_g,
                     web_u32_getter_t get_reminder_interval_ms,
                     web_u32_setter_t set_reminder_interval_ms,
                     web_action_t reset_total_drink);
void web_config_start_server(void);
float web_config_load_target_ml(void);
void web_config_save_target_ml(float target_ml);
uint32_t web_config_load_reminder_interval_ms(void);
void web_config_save_reminder_interval_ms(uint32_t reminder_interval_ms);
