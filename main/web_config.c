#include "web_config.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "nvs.h"

#define NVS_NAMESPACE "coaster_cfg"
#define NVS_KEY_TARGET "target_ml"
#define NVS_KEY_REMINDER "reminder_ms"
#define NVS_KEY_WIFI_SSID "wifi_ssid"
#define NVS_KEY_WIFI_PASS "wifi_pass"

static const char *TAG = "web_config";

static web_getter_t s_get_target_ml;
static web_setter_t s_set_target_ml;
static web_getter_t s_get_total_drink_g;
static web_u32_getter_t s_get_reminder_interval_ms;
static web_u32_setter_t s_set_reminder_interval_ms;
static web_action_t s_reset_total_drink;
static httpd_handle_t s_server = NULL;
static bool s_config_mode = false;
static bool s_has_pending_wifi = false;
static char s_pending_ssid[33];
static char s_pending_password[65];

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static void url_decode(char *text)
{
    char *src = text;
    char *dst = text;

    while (*src != '\0') {
        if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else if (*src == '%' && src[1] != '\0' && src[2] != '\0') {
            int hi = hex_value(src[1]);
            int lo = hex_value(src[2]);
            if (hi >= 0 && lo >= 0) {
                *dst++ = (char)((hi << 4) | lo);
                src += 3;
            } else {
                *dst++ = *src++;
            }
        } else {
            *dst++ = *src++;
        }
    }

    *dst = '\0';
}

static int find_form_value(const char *body, const char *key, char *out, size_t out_size)
{
    const char *start = strstr(body, key);
    size_t i = 0;

    if (start == NULL || out_size == 0U) {
        return 0;
    }

    start += strlen(key);
    while (start[i] != '\0' && start[i] != '&' && i < out_size - 1U) {
        out[i] = start[i];
        i++;
    }
    out[i] = '\0';
    url_decode(out);
    return i > 0U;
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    char html[2600];
    char saved_ssid[33] = {0};
    char saved_password[65] = {0};
    float target_ml = s_get_target_ml ? s_get_target_ml() : DEFAULT_TARGET_ML;
    float total_g = s_get_total_drink_g ? s_get_total_drink_g() : 0.0f;
    uint32_t reminder_interval_ms = s_get_reminder_interval_ms ? s_get_reminder_interval_ms() : DEFAULT_REMINDER_INTERVAL_MS;
    uint32_t reminder_minutes = reminder_interval_ms / 60000U;
    int progress = (target_ml > 0.0f) ? (int)((total_g / target_ml) * 100.0f) : 0;

    web_config_load_wifi(saved_ssid, sizeof(saved_ssid), saved_password, sizeof(saved_password));

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
        "<style>body{font-family:Arial,sans-serif;max-width:380px;margin:24px auto;padding:0 12px;}"
        ".bar{height:18px;background:#ddd;border-radius:9px;overflow:hidden;}.fill{height:100%%;background:#2f7d32;}"
        "input,button{width:100%%;padding:10px;margin-top:8px;font-size:16px;box-sizing:border-box;}"
        ".card{padding:14px;border:1px solid #ddd;border-radius:12px;margin-top:16px;}</style></head>"
        "<body><h2>%s</h2><p>%s</p>"
        "<div class='card'><form method='post' action='/settings'>"
        "<label>WiFi SSID</label><input name='wifi_ssid' type='text' maxlength='32' value='%s'>"
        "<label>WiFi Password</label><input name='wifi_password' type='password' maxlength='64' value='%s'>"
        "<label>Daily target (mL)</label><input name='target_ml' type='number' min='100' max='5000' step='50' value='%.0f'>"
        "<label>Reminder interval (minutes)</label><input name='reminder_min' type='number' min='1' max='240' step='1' value='%lu'>"
        "<button type='submit'>Save Settings</button></form></div>"
        "<div class='card'><p>Total today: %.0f mL</p><p>Target: %.0f mL</p>"
        "<div class='bar'><div class='fill' style='width:%d%%'></div></div></div>"
        "<div class='card'><form method='post' action='/reset'><button type='submit'>Reset Today Intake</button></form></div>"
        "</body></html>",
        s_config_mode ? "Drink Coaster Setup" : "Drink Coaster",
        s_config_mode ? "Connect to this AP, save WiFi, and the device will switch to your router."
                      : "Open this page from your local network.",
        saved_ssid,
        saved_password,
        target_ml,
        (unsigned long)reminder_minutes,
        total_g,
        target_ml,
        progress);

    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t settings_post_handler(httpd_req_t *req)
{
    char content[256] = {0};
    char target_buf[16] = {0};
    char reminder_buf[16] = {0};
    char ssid_buf[33] = {0};
    char password_buf[65] = {0};
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

    find_form_value(content, "wifi_ssid=", ssid_buf, sizeof(ssid_buf));
    find_form_value(content, "wifi_password=", password_buf, sizeof(password_buf));

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

    if (ssid_buf[0] != '\0') {
        web_config_save_wifi(ssid_buf, password_buf);
        if (s_config_mode) {
            snprintf(s_pending_ssid, sizeof(s_pending_ssid), "%s", ssid_buf);
            snprintf(s_pending_password, sizeof(s_pending_password), "%s", password_buf);
            s_has_pending_wifi = true;
        }
    }

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

void web_config_start_server(bool config_mode)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = root_get_handler};
    httpd_uri_t settings = {.uri = "/settings", .method = HTTP_POST, .handler = settings_post_handler};
    httpd_uri_t reset = {.uri = "/reset", .method = HTTP_POST, .handler = reset_post_handler};

    if (s_server != NULL) {
        return;
    }

    s_config_mode = config_mode;
    s_has_pending_wifi = false;
    s_pending_ssid[0] = '\0';
    s_pending_password[0] = '\0';
    config.stack_size = 8192;
    ESP_ERROR_CHECK(httpd_start(&s_server, &config));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &root));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &settings));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &reset));
    ESP_LOGI(TAG, "HTTP config server started.");
}

void web_config_stop_server(void)
{
    if (s_server == NULL) {
        return;
    }

    httpd_stop(s_server);
    s_server = NULL;
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

bool web_config_load_wifi(char *ssid, size_t ssid_size, char *password, size_t password_size)
{
    nvs_handle_t nvs_handle;
    bool ok = false;

    if (ssid != NULL && ssid_size > 0U) {
        ssid[0] = '\0';
    }
    if (password != NULL && password_size > 0U) {
        password[0] = '\0';
    }

    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle) == ESP_OK) {
        size_t required_size = ssid_size;
        if (ssid != NULL && ssid_size > 0U &&
            nvs_get_str(nvs_handle, NVS_KEY_WIFI_SSID, ssid, &required_size) == ESP_OK) {
            ok = true;
        }

        required_size = password_size;
        if (password != NULL && password_size > 0U) {
            nvs_get_str(nvs_handle, NVS_KEY_WIFI_PASS, password, &required_size);
        }
        nvs_close(nvs_handle);
    }

    return ok;
}

void web_config_save_wifi(const char *ssid, const char *password)
{
    nvs_handle_t nvs_handle;

    if (ssid == NULL || ssid[0] == '\0') {
        return;
    }

    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_str(nvs_handle, NVS_KEY_WIFI_SSID, ssid);
        nvs_set_str(nvs_handle, NVS_KEY_WIFI_PASS, password ? password : "");
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }
}

bool web_config_consume_wifi_update(char *ssid, size_t ssid_size, char *password, size_t password_size)
{
    if (!s_has_pending_wifi) {
        return false;
    }

    if (ssid != NULL && ssid_size > 0U) {
        snprintf(ssid, ssid_size, "%s", s_pending_ssid);
    }
    if (password != NULL && password_size > 0U) {
        snprintf(password, password_size, "%s", s_pending_password);
    }

    s_pending_ssid[0] = '\0';
    s_pending_password[0] = '\0';
    s_has_pending_wifi = false;
    return true;
}
