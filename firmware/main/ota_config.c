#include "ota_config.h"
#include "ota_firmware.h"

#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "nvs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "edgehmi_ota";
static edgehmi_bus_t *s_bus = NULL;
static const edgehmi_config_t *s_active_cfg = NULL;
static const edgehmi_mb_stats_t *s_mb_stats = NULL;
static SemaphoreHandle_t s_cfg_mutex = NULL;
static edgehmi_reload_cb s_reload_cb = NULL;

void *edgehmi_alloc_buf(size_t size) {
    if (size == 0) return NULL;
    void *ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ptr) {
        ptr = malloc(size);
    }
    return ptr;
}

edgehmi_err_t edgehmi_storage_init(void) {
    esp_vfs_littlefs_conf_t conf = {
        .base_path = EDGEHMI_FS_BASE,
        .partition_label = "cfg_fs",
        .format_if_mount_failed = true,
        .dont_mount = false,
    };
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LittleFS mount failed: %d", (int)err);
        return EDGEHMI_ERR_IO;
    }
    return EDGEHMI_OK;
}

bool edgehmi_boot_begin_check(void) {
    nvs_handle_t h;
    uint8_t fails = 0;
    bool rolled_back = false;

    if (nvs_open("edgehmi_boot", NVS_READWRITE, &h) == ESP_OK) {
        nvs_get_u8(h, "fail_cnt", &fails);
        if (fails >= 2) {
            FILE *bk = fopen(EDGEHMI_BACKUP_PATH, "rb");
            if (bk) {
                fclose(bk);
                ESP_LOGW(TAG, "Detected %u consecutive crash boots — rolling back to backup.json!",
                         (unsigned)fails);
                remove(EDGEHMI_ACTIVE_PATH);
                rename(EDGEHMI_BACKUP_PATH, EDGEHMI_ACTIVE_PATH);
                fails = 0;
                rolled_back = true;
            }
        }
        fails++;
        nvs_set_u8(h, "fail_cnt", fails);
        nvs_commit(h);
        nvs_close(h);
    }
    return rolled_back;
}

void edgehmi_boot_mark_healthy(void) {
    nvs_handle_t h;
    if (nvs_open("edgehmi_boot", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "fail_cnt", 0);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "Boot marked HEALTHY (fail_cnt=0)");
    }
}

static edgehmi_err_t read_file_buf(const char *path, char *out_buf, size_t cap, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return EDGEHMI_ERR_NOT_FOUND;
    size_t n = fread(out_buf, 1, cap - 1, f);
    fclose(f);
    out_buf[n] = '\0';
    if (out_len) *out_len = n;
    return EDGEHMI_OK;
}

edgehmi_err_t edgehmi_storage_read_active(char *out_buf, size_t cap, size_t *out_len) {
    if (read_file_buf(EDGEHMI_ACTIVE_PATH, out_buf, cap, out_len) == EDGEHMI_OK) {
        return EDGEHMI_OK;
    }
    ESP_LOGW(TAG, "active.json missing; falling back to backup.json");
    return read_file_buf(EDGEHMI_BACKUP_PATH, out_buf, cap, out_len);
}

edgehmi_err_t edgehmi_storage_save_active(const char *json_buf, size_t len) {
    if (!json_buf || len == 0) {
        return EDGEHMI_ERR_INVALID_ARG;
    }
    FILE *f = fopen(EDGEHMI_STAGING_PATH, "wb");
    if (!f) {
        return EDGEHMI_ERR_IO;
    }
    size_t written = fwrite(json_buf, 1, len, f);
    fclose(f);
    if (written != len) {
        remove(EDGEHMI_STAGING_PATH);
        return EDGEHMI_ERR_IO;
    }
    remove(EDGEHMI_BACKUP_PATH);
    rename(EDGEHMI_ACTIVE_PATH, EDGEHMI_BACKUP_PATH);
    if (rename(EDGEHMI_STAGING_PATH, EDGEHMI_ACTIVE_PATH) != 0) {
        return EDGEHMI_ERR_IO;
    }
    return EDGEHMI_OK;
}

static bool check_bearer_auth(httpd_req_t *req) {
    char token[64] = {0};
    if (s_cfg_mutex) xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
    if (s_active_cfg && s_active_cfg->api_token[0] != '\0') {
        strncpy(token, s_active_cfg->api_token, sizeof(token) - 1);
    }
    if (s_cfg_mutex) xSemaphoreGive(s_cfg_mutex);

    if (token[0] == '\0') {
        return true; /* Dev mode: no token configured */
    }
    char hdr[128] = {0};
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) != ESP_OK) {
        return false;
    }
    char expected[96];
    snprintf(expected, sizeof(expected), "Bearer %s", token);
    return edgehmi_ct_str_eq(hdr, expected);
}

static esp_err_t handle_health(httpd_req_t *req) {
    char resp[256];
    edgehmi_mb_stats_t stats_copy;
    bool has_stats = false;
    if (s_mb_stats) {
        if (s_cfg_mutex) xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
        stats_copy = *s_mb_stats;
        has_stats = true;
        if (s_cfg_mutex) xSemaphoreGive(s_cfg_mutex);
    }
    if (has_stats) {
        snprintf(resp, sizeof(resp),
                 "{\"status\":\"ok\",\"engine\":\"edgehmi-c11\","
                 "\"modbus\":{\"tx\":%lu,\"rx_ok\":%lu,\"crc_err\":%lu,"
                 "\"timeout_err\":%lu,\"retries\":%lu,\"last_rtt_ms\":%lu}}",
                 (unsigned long)stats_copy.tx_count,
                 (unsigned long)stats_copy.rx_ok,
                 (unsigned long)stats_copy.crc_err,
                 (unsigned long)stats_copy.timeout_err,
                 (unsigned long)stats_copy.retry_count,
                 (unsigned long)stats_copy.last_rtt_ms);
    } else {
        snprintf(resp, sizeof(resp), "{\"status\":\"ok\",\"engine\":\"edgehmi-c11\"}");
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, resp);
}

static esp_err_t handle_get_tags(httpd_req_t *req) {
    if (!s_bus) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "bus not ready");
    }
    char *buf = (char *)edgehmi_alloc_buf(8192);
    if (!buf) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }
    size_t pos = (size_t)snprintf(buf, 8192, "{\"tags\":[");
    if (s_cfg_mutex) xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
    for (size_t i = 0; i < s_bus->count && pos < 7900; ++i) {
        double v = 0.0;
        edgehmi_quality_t q = TAG_QUAL_INIT;
        edgehmi_bus_get(s_bus, (int16_t)i, &v, &q);
        char esc_name[64];
        edgehmi_json_escape(s_bus->tags[i].name, esc_name, sizeof(esc_name));
        pos += (size_t)snprintf(
            buf + pos, 8192 - pos,
            "%s{\"name\":\"%s\",\"value\":%.4f,\"quality\":%d}",
            (i > 0) ? "," : "", esc_name, v, (int)q);
    }
    if (s_cfg_mutex) xSemaphoreGive(s_cfg_mutex);
    snprintf(buf + pos, 8192 - pos, "]}");
    httpd_resp_set_type(req, "application/json");
    esp_err_t rc = httpd_resp_sendstr(req, buf);
    free(buf);
    return rc;
}

static esp_err_t handle_post_config(httpd_req_t *req) {
    if (!check_bearer_auth(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"missing or invalid Bearer token\"}");
    }
    if (req->content_len <= 0 || req->content_len > 65536) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid config size (max 64KB)");
    }

    char *body = (char *)edgehmi_alloc_buf((size_t)req->content_len + 1);
    if (!body) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }

    int received = 0;
    while (received < req->content_len) {
        int r = httpd_req_recv(req, body + received, req->content_len - received);
        if (r <= 0) {
            free(body);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
        }
        received += r;
    }
    body[received] = '\0';

    /* 1. Dry-run validate & hot-reload */
    char err_msg[128] = {0};
    edgehmi_err_t rc = s_reload_cb ? s_reload_cb(body, err_msg, sizeof(err_msg)) : EDGEHMI_ERR_IO;
    if (rc != EDGEHMI_OK) {
        free(body);
        char esc_err[192];
        edgehmi_json_escape(err_msg, esc_err, sizeof(esc_err));
        char resp[256];
        snprintf(resp, sizeof(resp), "{\"ok\":false,\"error\":\"%s\"}", esc_err);
        httpd_resp_set_status(req, "422 Unprocessable Entity");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, resp);
    }

    /* 2. Atomic persist: staging -> backup -> active */
    edgehmi_err_t save_rc = edgehmi_storage_save_active(body, (size_t)received);
    free(body);
    if (save_rc != EDGEHMI_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "config persist failed");
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true,\"status\":\"reloaded\"}");
}

edgehmi_err_t edgehmi_http_start(edgehmi_bus_t *bus,
                                 const edgehmi_config_t *active_cfg,
                                 const edgehmi_mb_stats_t *mb_stats,
                                 SemaphoreHandle_t cfg_mutex,
                                 edgehmi_reload_cb reload_cb) {
    s_bus = bus;
    s_active_cfg = active_cfg;
    s_mb_stats = mb_stats;
    s_cfg_mutex = cfg_mutex;
    s_reload_cb = reload_cb;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.core_id = 0; /* Pin HTTP/OTA management server to Core 0 */
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        return EDGEHMI_ERR_IO;
    }

    httpd_uri_t u_health = { .uri = "/api/health", .method = HTTP_GET, .handler = handle_health };
    httpd_uri_t u_tags   = { .uri = "/api/tags",   .method = HTTP_GET, .handler = handle_get_tags };
    httpd_uri_t u_cfg    = { .uri = "/api/config", .method = HTTP_POST, .handler = handle_post_config };

    httpd_register_uri_handler(server, &u_health);
    httpd_register_uri_handler(server, &u_tags);
    httpd_register_uri_handler(server, &u_cfg);
    edgehmi_ota_register_handlers(server, active_cfg, cfg_mutex);

    ESP_LOGI(TAG, "Config + Firmware OTA HTTP server started on Core 0 (port %d)", config.server_port);
    return EDGEHMI_OK;
}
