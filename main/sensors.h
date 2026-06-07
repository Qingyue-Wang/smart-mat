#pragma once

#include <stdbool.h>
#include <stdint.h>

// 传感器模块：负责 HX711 称重读取和 TCRT5000 杯子在位检测。
void sensors_init(void);
void sensors_tare(void);
int32_t sensors_get_raw_average(int samples);
int32_t sensors_get_offset(void);
float sensors_get_weight_grams(int samples);
bool sensors_has_cup(void);
