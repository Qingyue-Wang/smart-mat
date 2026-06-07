#include "wifi_manager.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_DISCONNECTED_BIT BIT1

static const char *TAG = "wifi_manager";
// 事件组用于等待联网成功或失败，避免主流程盲等。
static EventGroupHandle_t s_wifi_events;
static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif = NULL;
static bool s_initialized = false;
static bool s_wifi_connected = false;
static bool s_ap_mode = false;
static bool s_time_synced = false;
static char s_ip_text[32] = "not-connected";

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;

    // STA 断开、AP 启停、STA 获取 IP 都在这里统一更新状态。
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_connected = false;
        s_time_synced = false;
        s_ap_mode = false;
        snprintf(s_ip_text, sizeof(s_ip_text), "not-connected");
        xEventGroupSetBits(s_wifi_events, WIFI_DISCONNECTED_BIT);
        ESP_LOGW(TAG, "STA disconnected");
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_START) {
        s_ap_mode = true;
        s_wifi_connected = false;
        s_time_synced = false;
        snprintf(s_ip_text, sizeof(s_ip_text), "192.168.4.1");
        ESP_LOGI(TAG, "SoftAP started at %s", s_ip_text);
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STOP) {
        s_ap_mode = false;
        snprintf(s_ip_text, sizeof(s_ip_text), "not-connected");
        ESP_LOGI(TAG, "SoftAP stopped");
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        s_wifi_connected = true;
        s_ap_mode = false;
        snprintf(s_ip_text, sizeof(s_ip_text), IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupClearBits(s_wifi_events, WIFI_DISCONNECTED_BIT);
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "Got STA IP: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

void wifi_manager_init(void)
{
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

    // 只初始化一次网络栈、事件循环和 WiFi 驱动。
    if (s_initialized) {
        return;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    s_wifi_events = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    s_initialized = true;
}

static void wifi_manager_clear_state(void)
{
    // 清空运行期状态，避免 STA/AP 模式切换时残留旧信息。
    s_wifi_connected = false;
    s_ap_mode = false;
    s_time_synced = false;
    snprintf(s_ip_text, sizeof(s_ip_text), "not-connected");
    xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_DISCONNECTED_BIT);
}

void wifi_manager_stop(void)
{
    if (!s_initialized) {
        return;
    }

    esp_wifi_stop();
    wifi_manager_clear_state();
}

bool wifi_manager_connect_sta(const char *ssid, const char *password, uint32_t timeout_ms)
{
    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    EventBits_t bits;

    if (!s_initialized || ssid == NULL || ssid[0] == '\0') {
        return false;
    }

    // 进入 STA 模式前先停掉当前 WiFi，保证 AP 和 STA 不混用。
    wifi_manager_stop();
    wifi_manager_clear_state();

    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, password ? password : "",
            sizeof(wifi_config.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_connect());

    ESP_LOGI(TAG, "Connecting to WiFi SSID: %s", ssid);

    // 等待联网结果，超时则认为这次连接失败。
    bits = xEventGroupWaitBits(
        s_wifi_events,
        WIFI_CONNECTED_BIT | WIFI_DISCONNECTED_BIT,
        pdTRUE,
        pdFALSE,
        pdMS_TO_TICKS(timeout_ms));

    if ((bits & WIFI_CONNECTED_BIT) != 0U) {
        return true;
    }

    ESP_LOGW(TAG, "WiFi connect timeout/failure for SSID: %s", ssid);
    wifi_manager_stop();
    return false;
}

void wifi_manager_start_ap(const char *ssid, const char *password)
{
    wifi_config_t wifi_config = {
        .ap = {
            .ssid_len = 0,
            .channel = 1,
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK,
        },
    };

    if (!s_initialized) {
        return;
    }

    // 进入 AP 配网前同样先清掉当前 WiFi 运行状态。
    wifi_manager_stop();
    wifi_manager_clear_state();

    strncpy((char *)wifi_config.ap.ssid, ssid, sizeof(wifi_config.ap.ssid) - 1);
    strncpy((char *)wifi_config.ap.password, password ? password : "",
            sizeof(wifi_config.ap.password) - 1);
    wifi_config.ap.ssid_len = strlen((char *)wifi_config.ap.ssid);

    if (wifi_config.ap.password[0] == '\0') {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

bool wifi_manager_is_connected(void)
{
    return s_wifi_connected;
}

bool wifi_manager_is_ap_mode(void)
{
    return s_ap_mode;
}

void wifi_manager_get_ip_string(char *buffer, size_t buffer_size)
{
    if (buffer == NULL || buffer_size == 0U) {
        return;
    }

    snprintf(buffer, buffer_size, "%s", s_ip_text);
}

bool wifi_manager_sync_time(uint32_t timeout_ms)
{
    time_t now = 0;
    uint32_t waited_ms = 0;

    if (!s_wifi_connected) {
        return false;
    }

    // 通过 SNTP 从网络获取当前时间，供 OLED 显示和业务使用。
    setenv("TZ", "CST-8", 1);
    tzset();

    esp_sntp_stop();
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();

    while (waited_ms < timeout_ms) {
        time(&now);
        if (now > 1700000000) {
            s_time_synced = true;
            ESP_LOGI(TAG, "Time synchronized");
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
        waited_ms += 500;
    }

    ESP_LOGW(TAG, "Time sync timeout");
    return false;
}

bool wifi_manager_has_valid_time(void)
{
    return s_time_synced;
}

void wifi_manager_get_time_string(char *buffer, size_t buffer_size)
{
    time_t now;
    struct tm timeinfo = {0};

    if (buffer == NULL || buffer_size == 0U) {
        return;
    }

    if (!s_time_synced) {
        // 没对时成功时，用占位文本代替真实时间。
        snprintf(buffer, buffer_size, "--:--");
        return;
    }

    time(&now);
    localtime_r(&now, &timeinfo);
    strftime(buffer, buffer_size, "%H:%M", &timeinfo);
}
