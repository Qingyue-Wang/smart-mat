#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void wifi_manager_init(void);
bool wifi_manager_connect_sta(const char *ssid, const char *password, uint32_t timeout_ms);
void wifi_manager_start_ap(const char *ssid, const char *password);
void wifi_manager_stop(void);
bool wifi_manager_is_connected(void);
bool wifi_manager_is_ap_mode(void);
void wifi_manager_get_ip_string(char *buffer, size_t buffer_size);
bool wifi_manager_sync_time(uint32_t timeout_ms);
bool wifi_manager_has_valid_time(void);
void wifi_manager_get_time_string(char *buffer, size_t buffer_size);
