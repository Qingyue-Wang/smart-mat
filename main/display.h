#pragma once

#include "app_state.h"

// 显示模块：负责 OLED 初始化、启动页和常态页显示。
void display_init(void);
void display_show_boot(const char *line1, const char *line2);
void display_show_status(const app_status_t *status);
