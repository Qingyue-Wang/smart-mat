#pragma once

// 默认联网参数。若网页已经保存过 WiFi，运行时会优先使用 NVS 中的配置。
#define WIFI_STA_SSID "mooncell"
#define WIFI_STA_PASSWORD "homuramadoka"

// 巴法云 UID，用于把提醒推送到绑定的微信账号。
#define BEMFA_UID "ec5d3b7cdb267aac72c5cb2f9f2f7c34"

// 设备名称会直接进入 HTTP JSON 文本，使用 ASCII 最稳妥。
#define BEMFA_DEVICE_NAME "smart_coaster"
