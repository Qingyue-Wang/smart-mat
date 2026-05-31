#include "web_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "nvs.h"

#define NVS_NAMESPACE "coaster_cfg"
#define NVS_KEY_TARGET "target_ml"
#define NVS_KEY_REMINDER "reminder_ms"

static const char *TAG = "web_config";

static web_getter_t s_get_target_ml;
static web_setter_t s_set_target_ml;
static web_getter_t s_get_total_drink_g;
static web_u32_getter_t s_get_reminder_interval_ms;
static web_u32_setter_t s_set_reminder_interval_ms;
static web_action_t s_reset_total_drink;
static httpd_handle_t s_server = NULL;

static int find_form_value(const char *body, const char *key, char *out, size_t out_size)
{
    const char *start = strstr(body, key);
    size_t i = 0;

    if (start == NULL || out_size == 0) {
        return 0;
    }

    start += strlen(key);
    while (start[i] != '\0' && start[i] != '&' && i < out_size - 1) {
        out[i] = start[i];
        i++;
    }
    out[i] = '\0';
    return i > 0;
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    char html[1600];
    float target_ml = s_get_target_ml ? s_get_target_ml() : DEFAULT_TARGET_ML;
    float total_g = s_get_total_drink_g ? s_get_total_drink_g() : 0.0f;
    uint32_t reminder_interval_ms = s_get_reminder_interval_ms ? s_get_reminder_interval_ms() : DEFAULT_REMINDER_INTERVAL_MS;
    uint32_t reminder_minutes = reminder_interval_ms / 60000U;
    int progress = (target_ml > 0.0f) ? (int)((total_g / target_ml) * 100.0f) : 0;

    if (progress < 0) {
        progress = 0;
    }
    if (progress > 100) {
        progress = 100;
    }

    snprintf(
        html,
        sizeof(html),
        "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' "
        "content='width=device-width, initial-scale=1'>"
        "<title>Drink Coaster</title>"
        "<style>body{font-family:Arial,sans-serif;max-width:360px;margin:24px auto;padding:0 12px;}"
        ".bar{height:18px;background:#ddd;border-radius:9px;overflow:hidden;}.fill{height:100%%;background:#2f7d32;}"
        "input,button{width:100%%;padding:10px;margin-top:8px;font-size:16px;}small{color:#555;}"
        ".card{padding:14px;border:1px solid #ddd;border-radius:12px;margin-top:16px;}</style></head>"
        "<body><h2>Drink Coaster</h2><p>Open this page from your local network.</p>"
        "<p>Total today: %.0f mL</p><p>Target: %.0f mL</p>"
        "<div class='bar'><div class='fill' style='width:%d%%'></div></div>"
        "<div class='card'><form method='post' action='/settings'><label>Daily target (mL)</label>"
        "<input name='target_ml' type='number' min='100' max='5000' step='50' value='%.0f'>"
        "<label>Reminder interval (minutes)</label>"
        "<input name='reminder_min' type='number' min='1' max='240' step='1' value='%lu'>"
        "<button type='submit'>Save Settings</button></form></div>"
        "<div class='card'><form method='post' action='/reset'>"
        "<button type='submit'>Reset Today Intake</button></form></div></body></html>",
        total_g, target_ml, progress, target_ml, (unsigned long)reminder_minutes);

    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t settings_post_handler(httpd_req_t *req)
{
    char content[128] = {0};
    char target_buf[16] = {0};
    char reminder_buf[16] = {0};
    int recv_len = httpd_req_recv(req, content, sizeof(content) - 1);

    if (recv_len <= 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid body");
    }

    if (!find_form_value(content, "target_ml=", target_buf, sizeof(target_buf))) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing target_ml");
    }
    if (!find_form_value(content, "reminder_min=", reminder_buf, sizeof(reminder_buf))) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing reminder_min");
    }

    float target_ml = (float)atoi(target_buf);
    uint32_t reminder_min = (uint32_t)atoi(reminder_buf);
    if (target_ml < 100.0f || target_ml > 5000.0f) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "target out of range");
    }
    if (reminder_min < 1U || reminder_min > 240U) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "reminder out of range");
    }

    if (s_set_target_ml) {
        s_set_target_ml(target_ml);
    }
    if (s_set_reminder_interval_ms) {
        s_set_reminder_interval_ms(reminder_min * 60000U);
    }
    web_config_save_target_ml(target_ml);
    web_config_save_reminder_interval_ms(reminder_min * 60000U);

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t reset_post_handler(httpd_req_t *req)
{
    if (s_reset_total_drink) {
        s_reset_total_drink();
    }

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    return httpd_resp_send(req, NULL, 0);
}

void web_config_start_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = root_get_handler};
    httpd_uri_t settings = {.uri = "/settings", .method = HTTP_POST, .handler = settings_post_handler};
    httpd_uri_t reset = {.uri = "/reset", .method = HTTP_POST, .handler = reset_post_handler};

    if (s_server != NULL) {
        return;
    }

    ESP_ERROR_CHECK(httpd_start(&s_server, &config));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &root));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &settings));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &reset));
    ESP_LOGI(TAG, "HTTP config server started on current LAN IP.");
}

void web_config_init(web_getter_t get_target_ml,
                     web_setter_t set_target_ml,
                     web_getter_t get_total_drink_g,
                     web_u32_getter_t get_reminder_interval_ms,
                     web_u32_setter_t set_reminder_interval_ms,
                     web_action_t reset_total_drink)
{
    s_get_target_ml = get_target_ml;
    s_set_target_ml = set_target_ml;
    s_get_total_drink_g = get_total_drink_g;
    s_get_reminder_interval_ms = get_reminder_interval_ms;
    s_set_reminder_interval_ms = set_reminder_interval_ms;
    s_reset_total_drink = reset_total_drink;
}

float web_config_load_target_ml(void)
{
    nvs_handle_t nvs_handle;
    float target_ml = DEFAULT_TARGET_ML;

    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle) == ESP_OK) {
        size_t required_size = sizeof(target_ml);
        if (nvs_get_blob(nvs_handle, NVS_KEY_TARGET, &target_ml, &required_size) != ESP_OK) {
            target_ml = DEFAULT_TARGET_ML;
        }
        nvs_close(nvs_handle);
    }

    return target_ml;
}

void web_config_save_target_ml(float target_ml)
{
    nvs_handle_t nvs_handle;

    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_blob(nvs_handle, NVS_KEY_TARGET, &target_ml, sizeof(target_ml));
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }
}

uint32_t web_config_load_reminder_interval_ms(void)
{
    nvs_handle_t nvs_handle;
    uint32_t reminder_interval_ms = DEFAULT_REMINDER_INTERVAL_MS;

    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle) == ESP_OK) {
        size_t required_size = sizeof(reminder_interval_ms);
        if (nvs_get_blob(nvs_handle, NVS_KEY_REMINDER, &reminder_interval_ms, &required_size) != ESP_OK) {
            reminder_interval_ms = DEFAULT_REMINDER_INTERVAL_MS;
        }
        nvs_close(nvs_handle);
    }

    return reminder_interval_ms;
}

void web_config_save_reminder_interval_ms(uint32_t reminder_interval_ms)
{
    nvs_handle_t nvs_handle;

    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_blob(nvs_handle, NVS_KEY_REMINDER, &reminder_interval_ms, sizeof(reminder_interval_ms));
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }
}
