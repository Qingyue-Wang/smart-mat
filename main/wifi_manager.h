#pragma once

#include <stdbool.h>
#include <stddef.h>

void wifi_manager_init_sta(const char *ssid, const char *password);
bool wifi_manager_is_connected(void);
void wifi_manager_get_ip_string(char *buffer, size_t buffer_size);
