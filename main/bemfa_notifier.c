#include "bemfa_notifier.h"

#include <stdio.h>
#include <string.h>

#include "esp_http_client.h"
#include "esp_log.h"
#include "network_settings.h"

static const char *TAG = "bemfa";
static const char *BEMFA_WARN_URL = "http://apis.bemfa.com/vb/wechat/v1/wechatWarnJson";

static bool bemfa_send_message(const char *message)
{
    char body[384];
    esp_http_client_config_t config = {
        .url = BEMFA_WARN_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 5000,
    };

    int body_len = snprintf(body, sizeof(body),
                            "{\"uid\":\"%s\",\"device\":\"%s\",\"message\":\"%s\"}",
                            BEMFA_UID, BEMFA_DEVICE_NAME, message);

    if (body_len <= 0 || body_len >= (int)sizeof(body)) {
        ESP_LOGE(TAG, "Bemfa body too long");
        return false;
    }

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        return false;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json; charset=utf-8");
    esp_http_client_set_post_field(client, body, body_len);

    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Bemfa push failed: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "Bemfa push status: %d", status_code);
    return status_code >= 200 && status_code < 300;
}

bool bemfa_notifier_send_reminder(float total_drink_g, float target_ml, unsigned int interval_minutes)
{
    char message[192];

    snprintf(message, sizeof(message),
             "Time to drink water. Total %.0f mL, target %.0f mL, interval %u min.",
             total_drink_g, target_ml, interval_minutes);

    return bemfa_send_message(message);
}
