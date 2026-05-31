#pragma once

#include "app_state.h"

void display_init(void);
void display_show_boot(const char *line1, const char *line2);
void display_show_status(const app_status_t *status);
