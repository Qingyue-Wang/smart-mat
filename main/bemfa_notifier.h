#pragma once

#include <stdbool.h>

// 巴法云提醒模块：负责向微信发送饮水提醒。
bool bemfa_notifier_send_reminder(float total_drink_g, float target_ml, unsigned int interval_minutes);
