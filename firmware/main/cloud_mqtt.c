#include "cloud_mqtt.h"

#include "cJSON.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "nvs.h"
#include "ota_firmware.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "edgehmi_cloud";

#define MQTT_OFFLINE_Q_CAP  16
#define MQTT_MSG_MAX_LEN    512

typedef struct {
    char topic[96];
    char payload[MQTT_MSG_MAX_LEN];
} mqtt_queued_msg_t;

static const edgehmi_config_t *s_cfg = NULL;
static edgehmi_bus_t *s_bus = NULL;
static SemaphoreHandle_t s_cfg_mutex = NULL;
static esp_mqtt_client_handle_t s_client = NULL;
static volatile bool s_mqtt_connected = false;
static char s_dev_id[32] = "edgehmi-000000";

/* Static offline store-and-forward ring buffer (zero dynamic heap fragmentation) */
static mqtt_queued_msg_t s_offline_q[MQTT_OFFLINE_Q_CAP];
static size_t s_oq_head = 0;
static size_t s_oq_count = 0;
static SemaphoreHandle_t s_oq_mutex = NULL;

static void offline_enqueue(const char *topic, const char *payload) {
    if (!s_oq_mutex) return;
    xSemaphoreTake(s_oq_mutex, portMAX_DELAY);
    size_t idx = (s_oq_head + s_oq_count) % MQTT_OFFLINE_Q_CAP;
    if (s_oq_count == MQTT_OFFLINE_Q_CAP) {
        s_oq_head = (s_oq_head + 1u) % MQTT_OFFLINE_Q_CAP; /* Overwrite oldest */
    } else {
        s_oq_count++;
    }
    edgehmi_copy_str(s_offline_q[idx].topic, sizeof(s_offline_q[idx].topic), topic);
    edgehmi_copy_str(s_offline_q[idx].payload, sizeof(s_offline_q[idx].payload), payload);
    xSemaphoreGive(s_oq_mutex);
}

static void offline_flush(void) {
    if (!s_oq_mutex || !s_client || !s_mqtt_connected) return;
    xSemaphoreTake(s_oq_mutex, portMAX_DELAY);
    while (s_oq_count > 0 && s_mqtt_connected) {
        const mqtt_queued_msg_t *m = &s_offline_q[s_oq_head];
        esp_mqtt_client_publish(s_client, m->topic, m->payload, 0, 1, 0);
        s_oq_head = (s_oq_head + 1u) % MQTT_OFFLINE_Q_CAP;
        s_oq_count--;
    }
    xSemaphoreGive(s_oq_mutex);
}

static void publish_or_queue(const char *topic, const char *payload) {
    if (s_client && s_mqtt_connected) {
        esp_mqtt_client_publish(s_client, topic, payload, 0, 1, 0);
    } else {
        offline_enqueue(topic, payload);
    }
}

static void mqtt_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data) {
    (void)arg;
    (void)base;
    esp_mqtt_event_handle_t ev = (esp_mqtt_event_handle_t)event_data;
    if (event_id == MQTT_EVENT_CONNECTED) {
        s_mqtt_connected = true;
        char sub_topic[128];
        char prefix[48] = "edgehmi";
        if (s_cfg_mutex) xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
        if (s_cfg && s_cfg->cloud.topic_prefix[0]) {
            edgehmi_copy_str(prefix, sizeof(prefix), s_cfg->cloud.topic_prefix);
        }
        if (s_cfg_mutex) xSemaphoreGive(s_cfg_mutex);
        snprintf(sub_topic, sizeof(sub_topic), "%s/%s/cmd/write", prefix, s_dev_id);
        esp_mqtt_client_subscribe(s_client, sub_topic, 1);
        snprintf(sub_topic, sizeof(sub_topic), "%s/%s/cmd/ota_pull", prefix, s_dev_id);
        esp_mqtt_client_subscribe(s_client, sub_topic, 1);
        ESP_LOGI(TAG, "MQTT connected; subscribed cmd/write & cmd/ota_pull (flushing backlog)");
        offline_flush();
    } else if (event_id == MQTT_EVENT_DISCONNECTED) {
        s_mqtt_connected = false;
    } else if (event_id == MQTT_EVENT_DATA && ev && ev->data_len > 0 && ev->data_len < 256) {
        char buf[257];
        memcpy(buf, ev->data, (size_t)ev->data_len);
        buf[ev->data_len] = '\0';
        cJSON *root = cJSON_Parse(buf);
        const cJSON *jurl = root ? cJSON_GetObjectItemCaseSensitive(root, "url") : NULL;
        if (cJSON_IsString(jurl) && ev->topic && strstr(ev->topic, "ota_pull") != NULL) {
            edgehmi_ota_trigger_pull(jurl->valuestring);
            cJSON_Delete(root);
            return;
        }
        const cJSON *jtag = root ? cJSON_GetObjectItemCaseSensitive(root, "tag") : NULL;
        const cJSON *jval = root ? cJSON_GetObjectItemCaseSensitive(root, "value") : NULL;
        if (cJSON_IsString(jtag) && cJSON_IsNumber(jval) && s_bus) {
            if (s_cfg_mutex) xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
            int16_t idx = edgehmi_bus_find(s_bus, jtag->valuestring);
            if (idx >= 0) {
                uint64_t ts = (uint64_t)(esp_timer_get_time() / 1000LL);
                edgehmi_bus_request_write(s_bus, idx, jval->valuedouble, ts);
                ESP_LOGI(TAG, "Remote MQTT write: %s = %.3f", jtag->valuestring, jval->valuedouble);
            }
            if (s_cfg_mutex) xSemaphoreGive(s_cfg_mutex);
        }
        cJSON_Delete(root);
    }
}

static void mqtt_telemetry_task(void *arg) {
    (void)arg;
    char payload[MQTT_MSG_MAX_LEN];
    char topic[96];

    for (;;) {
        uint32_t interval_ms = 5000;
        bool enabled = false;
        char prefix[48] = "edgehmi";

        if (s_cfg_mutex) xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
        if (s_cfg) {
            enabled = s_cfg->cloud.enabled && s_cfg->cloud.mqtt_uri[0] != '\0';
            if (s_cfg->cloud.publish_interval_ms >= 500) {
                interval_ms = s_cfg->cloud.publish_interval_ms;
            }
            if (s_cfg->cloud.topic_prefix[0]) {
                edgehmi_copy_str(prefix, sizeof(prefix), s_cfg->cloud.topic_prefix);
            }
        }

        if (enabled && s_bus) {
            size_t pos = (size_t)snprintf(payload, sizeof(payload), "{\"device\":\"%s\",\"tags\":{", s_dev_id);
            for (size_t i = 0; i < s_bus->count && pos < sizeof(payload) - 64; ++i) {
                double v = 0.0;
                edgehmi_bus_get(s_bus, (int16_t)i, &v, NULL);
                char esc[64];
                edgehmi_json_escape(s_bus->tags[i].name, esc, sizeof(esc));
                pos += (size_t)snprintf(payload + pos, sizeof(payload) - pos,
                                        "%s\"%s\":%.3f", (i > 0) ? "," : "", esc, v);
            }
            snprintf(payload + pos, sizeof(payload) - pos, "}}");
            snprintf(topic, sizeof(topic), "%s/%s/telemetry", prefix, s_dev_id);
        }
        if (s_cfg_mutex) xSemaphoreGive(s_cfg_mutex);

        if (enabled) {
            publish_or_queue(topic, payload);
        }
        vTaskDelay(pdMS_TO_TICKS(interval_ms));
    }
}

edgehmi_err_t edgehmi_net_save_wifi(const char *ssid, const char *password) {
    if (!ssid || ssid[0] == '\0') return EDGEHMI_ERR_INVALID_ARG;
    nvs_handle_t h;
    if (nvs_open("edgehmi_wifi", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "ssid", ssid);
        nvs_set_str(h, "pass", password ? password : "");
        nvs_commit(h);
        nvs_close(h);
    }
    wifi_config_t sta = {0};
    edgehmi_copy_str((char *)sta.sta.ssid, sizeof(sta.sta.ssid), ssid);
    if (password) edgehmi_copy_str((char *)sta.sta.password, sizeof(sta.sta.password), password);
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_STA, &sta);
    esp_wifi_disconnect();
    esp_wifi_connect();
    ESP_LOGI(TAG, "Saved Wi-Fi credentials for SSID '%s' & reconnecting", ssid);
    return EDGEHMI_OK;
}

edgehmi_err_t edgehmi_net_start(const edgehmi_config_t *cfg) {
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_dev_id, sizeof(s_dev_id), "edgehmi-%02x%02x%02x", mac[3], mac[4], mac[5]);

    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&wcfg) != ESP_OK) {
        return EDGEHMI_ERR_IO;
    }

    char ssid[33] = {0};
    char pass[65] = {0};
    nvs_handle_t h;
    if (nvs_open("edgehmi_wifi", NVS_READONLY, &h) == ESP_OK) {
        size_t sl = sizeof(ssid), pl = sizeof(pass);
        nvs_get_str(h, "ssid", ssid, &sl);
        nvs_get_str(h, "pass", pass, &pl);
        nvs_close(h);
    }
    if (ssid[0] == '\0' && cfg && cfg->net.ssid[0] != '\0') {
        edgehmi_copy_str(ssid, sizeof(ssid), cfg->net.ssid);
        edgehmi_copy_str(pass, sizeof(pass), cfg->net.password);
    }

    if (ssid[0] != '\0') {
        wifi_config_t sta = {0};
        edgehmi_copy_str((char *)sta.sta.ssid, sizeof(sta.sta.ssid), ssid);
        edgehmi_copy_str((char *)sta.sta.password, sizeof(sta.sta.password), pass);
        esp_wifi_set_mode((!cfg || cfg->net.ap_fallback) ? WIFI_MODE_APSTA : WIFI_MODE_STA);
        esp_wifi_set_config(WIFI_IF_STA, &sta);
    } else {
        esp_wifi_set_mode(WIFI_MODE_AP);
    }

    /* Provisioning SoftAP fallback: EdgeHMI-XXXX (config portal at http://192.168.4.1/setup) */
    if (!cfg || cfg->net.ap_fallback || ssid[0] == '\0') {
        wifi_config_t ap = {0};
        snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "EdgeHMI-%02X%02X", mac[4], mac[5]);
        edgehmi_copy_str((char *)ap.ap.password, sizeof(ap.ap.password), "edgehmi2026");
        ap.ap.ssid_len = (uint8_t)strlen((const char *)ap.ap.ssid);
        ap.ap.max_connection = 4;
        ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
        esp_wifi_set_config(WIFI_IF_AP, &ap);
    }

    esp_wifi_start();
    if (ssid[0] != '\0') {
        esp_wifi_connect();
    }

#ifdef CONFIG_EDGEHMI_ENABLE_4G_MODEM
    if (cfg && cfg->net.modem_4g_enabled) {
        ESP_LOGI(TAG, "4G Cat.1 PPP modem enabled on TX=%d RX=%d (esp_modem ready)",
                 (int)cfg->net.modem_tx_pin, (int)cfg->net.modem_rx_pin);
    }
#endif
    return EDGEHMI_OK;
}

void edgehmi_cloud_mqtt_reconfigure(void) {
    if (s_client) {
        s_mqtt_connected = false;
        esp_mqtt_client_stop(s_client);
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
    }
    edgehmi_cloud_cfg_t cpy;
    memset(&cpy, 0, sizeof(cpy));
    if (s_cfg_mutex) xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
    if (s_cfg) cpy = s_cfg->cloud;
    if (s_cfg_mutex) xSemaphoreGive(s_cfg_mutex);

    if (cpy.enabled && cpy.mqtt_uri[0] != '\0') {
        esp_mqtt_client_config_t mcfg = {
            .broker.address.uri = cpy.mqtt_uri,
            .credentials.client_id = cpy.client_id[0] ? cpy.client_id : s_dev_id,
            .credentials.username = cpy.username[0] ? cpy.username : NULL,
            .credentials.authentication.password = cpy.password[0] ? cpy.password : NULL,
        };
        s_client = esp_mqtt_client_init(&mcfg);
        if (s_client) {
            esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
            esp_mqtt_client_start(s_client);
        }
    }
}

edgehmi_err_t edgehmi_cloud_mqtt_start(const edgehmi_config_t *active_cfg,
                                       edgehmi_bus_t *bus,
                                       SemaphoreHandle_t cfg_mutex) {
    s_cfg = active_cfg;
    s_bus = bus;
    s_cfg_mutex = cfg_mutex;
    if (!s_oq_mutex) {
        s_oq_mutex = xSemaphoreCreateMutex();
    }
    edgehmi_cloud_mqtt_reconfigure();
    xTaskCreatePinnedToCore(mqtt_telemetry_task, "edgehmi_mqtt", 4096, NULL, 3, NULL, 0);
    return EDGEHMI_OK;
}

void edgehmi_cloud_mqtt_publish_alarm(const edgehmi_alarm_event_t *evt) {
    if (!evt || !s_cfg || !s_cfg->cloud.enabled) {
        return;
    }
    char topic[96];
    char payload[320];
    char esc_msg[140];
    edgehmi_json_escape(evt->message, esc_msg, sizeof(esc_msg));
    snprintf(topic, sizeof(topic), "%s/%s/alarm",
             s_cfg->cloud.topic_prefix[0] ? s_cfg->cloud.topic_prefix : "edgehmi", s_dev_id);
    snprintf(payload, sizeof(payload),
             "{\"rule\":\"%s\",\"level\":%d,\"active\":%s,\"ts_ms\":%llu,\"msg\":\"%s\"}",
             evt->rule_id, (int)evt->level, evt->active ? "true" : "false",
             (unsigned long long)evt->timestamp_ms, esc_msg);
    publish_or_queue(topic, payload);
}
