#pragma once

#include <stdbool.h>

bool bemfa_notifier_send_reminder(float total_drink_g, float target_ml, unsigned int interval_minutes);
