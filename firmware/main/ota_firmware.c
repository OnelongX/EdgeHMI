#include "ota_firmware.h"

#include "cJSON.h"
#include "data_bus.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ota_package.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char *TAG = "edgehmi_fw_ota";
static const edgehmi_config_t *s_cfg = NULL;
static SemaphoreHandle_t s_cfg_mutex = NULL;

static bool verify_bearer(httpd_req_t *req) {
    char token[64] = {0};
    if (s_cfg_mutex) xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
    if (s_cfg && s_cfg->api_token[0] != '\0') {
        strncpy(token, s_cfg->api_token, sizeof(token) - 1);
    }
    if (s_cfg_mutex) xSemaphoreGive(s_cfg_mutex);

    if (token[0] == '\0') return true;
    char hdr[128] = {0};
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) != ESP_OK) {
        return false;
    }
    char expected[96];
    snprintf(expected, sizeof(expected), "Bearer %s", token);
    return edgehmi_ct_str_eq(hdr, expected);
}

static void delayed_restart_task(void *arg) {
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

/* POST /api/ota/firmware — Streaming Push Firmware OTA with on-the-fly SHA-256 verification */
static esp_err_t handle_push_firmware(httpd_req_t *req) {
    if (!verify_bearer(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"unauthorized\"}");
    }
    if (req->content_len <= 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty firmware binary");
    }

    char expected_sha[65] = {0};
    bool check_sha = (httpd_req_get_hdr_value_str(req, "X-EdgeHMI-SHA256",
                                                  expected_sha, sizeof(expected_sha)) == ESP_OK &&
                      strlen(expected_sha) == 64);

    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no OTA partition");
    }

    esp_ota_handle_t ota_handle = 0;
    esp_err_t err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %d", (int)err);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota_begin failed");
    }

    uint8_t *chunk = (uint8_t *)malloc(4096);
    if (!chunk) {
        esp_ota_abort(ota_handle);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }

    edgehmi_sha256_ctx_t sha_ctx;
    edgehmi_sha256_init(&sha_ctx);

    int remaining = req->content_len;
    while (remaining > 0) {
        int to_read = remaining > 4096 ? 4096 : remaining;
        int r = httpd_req_recv(req, (char *)chunk, to_read);
        if (r <= 0) {
            free(chunk);
            esp_ota_abort(ota_handle);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
        }
        edgehmi_sha256_update(&sha_ctx, chunk, (size_t)r);
        if (esp_ota_write(ota_handle, chunk, (size_t)r) != ESP_OK) {
            free(chunk);
            esp_ota_abort(ota_handle);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota_write failed");
        }
        remaining -= r;
    }
    free(chunk);

    uint8_t digest[32];
    char hex[65];
    edgehmi_sha256_final(&sha_ctx, digest);
    edgehmi_sha256_to_hex(digest, hex);

    if (check_sha && strcasecmp(hex, expected_sha) != 0) {
        esp_ota_abort(ota_handle);
        ESP_LOGE(TAG, "SHA-256 mismatch: expected=%s got=%s", expected_sha, hex);
        httpd_resp_set_status(req, "422 Unprocessable Entity");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"sha256 mismatch\"}");
    }

    if (esp_ota_end(ota_handle) != ESP_OK || esp_ota_set_boot_partition(part) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota finalize failed");
    }

    ESP_LOGI(TAG, "Firmware OTA verified (sha256=%s), rebooting into %s...", hex, part->label);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"status\":\"rebooting\"}");
    xTaskCreatePinnedToCore(delayed_restart_task, "ota_reboot", 2048, NULL, 5, NULL, 0);
    return ESP_OK;
}

/* POST /api/ota/pull — Remote URL Pull Firmware OTA via esp_https_ota */
static esp_err_t handle_pull_firmware(httpd_req_t *req) {
    if (!verify_bearer(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"unauthorized\"}");
    }
    char body[512] = {0};
    if (req->content_len <= 0 || req->content_len >= (int)sizeof(body)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid payload");
    }
    int r = httpd_req_recv(req, body, req->content_len);
    if (r <= 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv failed");
    body[r] = '\0';

    cJSON *root = cJSON_Parse(body);
    const cJSON *jurl = root ? cJSON_GetObjectItemCaseSensitive(root, "url") : NULL;
    if (!cJSON_IsString(jurl)) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing url");
    }

    esp_http_client_config_t hcfg = {
        .url = jurl->valuestring,
        .timeout_ms = 15000,
    };
    esp_https_ota_config_t ota_cfg = {
        .http_config = &hcfg,
    };
    esp_err_t ret = esp_https_ota(&ota_cfg);
    cJSON_Delete(root);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Pull OTA failed: %d", (int)ret);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "pull ota failed");
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"status\":\"pull_ota_rebooting\"}");
    xTaskCreatePinnedToCore(delayed_restart_task, "ota_reboot", 2048, NULL, 5, NULL, 0);
    return ESP_OK;
}

edgehmi_err_t edgehmi_ota_register_handlers(httpd_handle_t server,
                                            const edgehmi_config_t *active_cfg,
                                            SemaphoreHandle_t cfg_mutex) {
    s_cfg = active_cfg;
    s_cfg_mutex = cfg_mutex;
    httpd_uri_t u_push = { .uri = "/api/ota/firmware", .method = HTTP_POST, .handler = handle_push_firmware };
    httpd_uri_t u_pull = { .uri = "/api/ota/pull",     .method = HTTP_POST, .handler = handle_pull_firmware };
    httpd_register_uri_handler(server, &u_push);
    httpd_register_uri_handler(server, &u_pull);
    return EDGEHMI_OK;
}

void edgehmi_ota_mark_app_valid(void) {
    esp_ota_mark_app_valid_cancel_rollback();
    ESP_LOGI(TAG, "ESP-IDF OTA image confirmed valid (rollback cancelled)");
}

static void mark_epk_applied(const char *epk_path) {
    char done_path[160];
    snprintf(done_path, sizeof(done_path), "%s.applied", epk_path);
    remove(done_path);
    if (rename(epk_path, done_path) != 0) {
        remove(epk_path);
    }
}

edgehmi_err_t edgehmi_ota_apply_offline_epk(const char *epk_path,
                                            edgehmi_reload_cb reload_cb) {
    FILE *f = fopen(epk_path, "rb");
    if (!f) return EDGEHMI_ERR_NOT_FOUND;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < EDGEHMI_EPK_HEADER_SIZE || sz > 2 * 1024 * 1024) {
        fclose(f);
        return EDGEHMI_ERR_PARSE;
    }

    uint8_t *buf = (uint8_t *)edgehmi_alloc_buf((size_t)sz);
    if (!buf) {
        fclose(f);
        return EDGEHMI_ERR_IO;
    }
    fread(buf, 1, (size_t)sz, f);
    fclose(f);

    edgehmi_epk_header_t hdr;
    const uint8_t *payload = NULL;
    edgehmi_err_t rc = edgehmi_epk_verify(buf, (size_t)sz, &hdr, &payload);
    if (rc != EDGEHMI_OK) {
        ESP_LOGE(TAG, "Offline EPK verification failed (%d)", (int)rc);
        free(buf);
        return rc;
    }

    if (hdr.pkg_type == EPK_TYPE_CONFIG && reload_cb) {
        char *jstr = (char *)edgehmi_alloc_buf(hdr.payload_len + 1);
        if (!jstr) {
            free(buf);
            return EDGEHMI_ERR_IO;
        }
        memcpy(jstr, payload, hdr.payload_len);
        jstr[hdr.payload_len] = '\0';
        char err[128] = {0};
        rc = reload_cb(jstr, err, sizeof(err));
        if (rc == EDGEHMI_OK) {
            rc = edgehmi_storage_save_active(jstr, hdr.payload_len);
            if (rc == EDGEHMI_OK) {
                mark_epk_applied(epk_path);
                ESP_LOGI(TAG, "Offline EPK config v%s applied & persisted", hdr.version);
            }
        } else {
            ESP_LOGE(TAG, "Offline EPK config rejected: %s", err);
        }
        free(jstr);
    } else if (hdr.pkg_type == EPK_TYPE_FIRMWARE) {
        const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
        esp_ota_handle_t h = 0;
        if (part && esp_ota_begin(part, hdr.payload_len, &h) == ESP_OK) {
            if (esp_ota_write(h, payload, hdr.payload_len) == ESP_OK &&
                esp_ota_end(h) == ESP_OK &&
                esp_ota_set_boot_partition(part) == ESP_OK) {
                ESP_LOGI(TAG, "Offline EPK firmware v%s flashed, rebooting...", hdr.version);
                free(buf);
                mark_epk_applied(epk_path);
                esp_restart();
            } else {
                esp_ota_abort(h);
                rc = EDGEHMI_ERR_IO;
            }
        }
    }

    free(buf);
    return rc;
}

static void async_pull_ota_task(void *arg) {
    char *url = (char *)arg;
    if (url && url[0]) {
        ESP_LOGI(TAG, "Starting remote MQTT Pull OTA from %s", url);
        esp_http_client_config_t hcfg = { .url = url, .timeout_ms = 20000 };
        esp_https_ota_config_t ota_cfg = { .http_config = &hcfg };
        if (esp_https_ota(&ota_cfg) == ESP_OK) {
            ESP_LOGI(TAG, "Remote Pull OTA succeeded, rebooting...");
            free(url);
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_restart();
        } else {
            ESP_LOGE(TAG, "Remote Pull OTA failed for %s", url);
        }
    }
    free(url);
    vTaskDelete(NULL);
}

edgehmi_err_t edgehmi_ota_trigger_pull(const char *url) {
    if (!url || strncmp(url, "http", 4) != 0) {
        return EDGEHMI_ERR_INVALID_ARG;
    }
    size_t len = strlen(url);
    char *copy = (char *)malloc(len + 1);
    if (!copy) return EDGEHMI_ERR_IO;
    memcpy(copy, url, len + 1);
    if (xTaskCreatePinnedToCore(async_pull_ota_task, "mqtt_ota", 8192, copy, 4, NULL, 0) != pdPASS) {
        free(copy);
        return EDGEHMI_ERR_IO;
    }
    return EDGEHMI_OK;
}
