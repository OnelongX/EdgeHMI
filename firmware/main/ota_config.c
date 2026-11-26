#include "ota_config.h"
#include "cellular_modem.h"
#include "cloud_mqtt.h"
#include "esp_timer.h"
#include "ota_firmware.h"
#include "recipe_sched.h"

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "nvs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "edgehmi_ota";
static edgehmi_bus_t *s_bus = NULL;
static const edgehmi_config_t *s_active_cfg = NULL;
static const edgehmi_mb_stats_t *s_mb_stats = NULL;
static edgehmi_history_t *s_history = NULL;
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

static bool recv_body_exact(httpd_req_t *req, char *buf, size_t cap) {
    if (!req || !buf || req->content_len <= 0 || (size_t)req->content_len >= cap) {
        return false;
    }
    int received = 0;
    while (received < req->content_len) {
        int r = httpd_req_recv(req, buf + received, req->content_len - received);
        if (r <= 0) {
            return false;
        }
        received += r;
    }
    buf[received] = '\0';
    return true;
}

static bool check_bearer_auth(httpd_req_t *req) {
    char token[64] = {0};
    if (s_cfg_mutex) xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
    if (s_active_cfg && s_active_cfg->api_token[0] != '\0') {
        edgehmi_copy_str(token, sizeof(token), s_active_cfg->api_token);
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
    char resp[512];
    edgehmi_mb_stats_t stats_copy;
    bool has_stats = false;
    if (s_mb_stats) {
        if (s_cfg_mutex) xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
        stats_copy = *s_mb_stats;
        has_stats = true;
        if (s_cfg_mutex) xSemaphoreGive(s_cfg_mutex);
    }
    edgehmi_cell_status_t cell;
    edgehmi_wan_link_t wan = WAN_LINK_OFFLINE;
    edgehmi_cellular_get_status(&cell, &wan);
    const char *wan_str = (wan == WAN_LINK_WIFI) ? "wifi"
                          : (wan == WAN_LINK_CELLULAR) ? "cellular"
                                                       : "offline";
    char esc_op[48];
    edgehmi_json_escape(cell.operator_name, esc_op, sizeof(esc_op));

    if (has_stats) {
        snprintf(resp, sizeof(resp),
                 "{\"status\":\"ok\",\"engine\":\"edgehmi-c11\",\"wan_link\":\"%s\","
                 "\"cellular\":{\"rat\":\"%s\",\"rssi_dbm\":%d,\"bars\":%u,"
                 "\"ppp_up\":%s,\"operator\":\"%s\"},"
                 "\"modbus\":{\"tx\":%lu,\"rx_ok\":%lu,\"crc_err\":%lu,"
                 "\"timeout_err\":%lu,\"retries\":%lu,\"last_rtt_ms\":%lu}}",
                 wan_str, edgehmi_cell_rat_str(cell.rat), (int)cell.rssi_dbm,
                 (unsigned)cell.signal_bars, cell.ppp_up ? "true" : "false", esc_op,
                 (unsigned long)stats_copy.tx_count,
                 (unsigned long)stats_copy.rx_ok,
                 (unsigned long)stats_copy.crc_err,
                 (unsigned long)stats_copy.timeout_err,
                 (unsigned long)stats_copy.retry_count,
                 (unsigned long)stats_copy.last_rtt_ms);
    } else {
        snprintf(resp, sizeof(resp),
                 "{\"status\":\"ok\",\"engine\":\"edgehmi-c11\",\"wan_link\":\"%s\"}", wan_str);
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

static esp_err_t handle_history_csv(httpd_req_t *req) {
    if (!s_history) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "history not ready");
    }
    char *buf = (char *)edgehmi_alloc_buf(32768);
    if (!buf) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }
    if (s_cfg_mutex) xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
    edgehmi_history_export_csv(s_history, buf, 32768);
    if (s_cfg_mutex) xSemaphoreGive(s_cfg_mutex);

    httpd_resp_set_type(req, "text/csv; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"edgehmi_history.csv\"");
    esp_err_t rc = httpd_resp_sendstr(req, buf);
    free(buf);
    return rc;
}

static esp_err_t handle_get_alarms(httpd_req_t *req) {
    if (!s_history) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "history not ready");
    }
    char *buf = (char *)edgehmi_alloc_buf(8192);
    if (!buf) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }
    if (s_cfg_mutex) xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
    edgehmi_history_export_alarms_json(s_history, buf, 8192);
    if (s_cfg_mutex) xSemaphoreGive(s_cfg_mutex);

    httpd_resp_set_type(req, "application/json");
    esp_err_t rc = httpd_resp_sendstr(req, buf);
    free(buf);
    return rc;
}

static esp_err_t handle_ack_alarms(httpd_req_t *req) {
    if (!check_bearer_auth(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"unauthorized\"}");
    }
    size_t acked = 0;
    if (s_history) {
        if (s_cfg_mutex) xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
        acked = edgehmi_history_ack_alarm(s_history, 0);
        if (s_cfg_mutex) xSemaphoreGive(s_cfg_mutex);
    }
    char resp[96];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"acked\":%u}", (unsigned)acked);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, resp);
}

static esp_err_t handle_get_audit(httpd_req_t *req) {
    if (!check_bearer_auth(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"unauthorized\"}");
    }
    if (!s_history) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "history not ready");
    }
    char *buf = (char *)edgehmi_alloc_buf(8192);
    if (!buf) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }
    if (s_cfg_mutex) xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
    edgehmi_history_export_audit_json(s_history, buf, 8192);
    if (s_cfg_mutex) xSemaphoreGive(s_cfg_mutex);

    httpd_resp_set_type(req, "application/json");
    esp_err_t rc = httpd_resp_sendstr(req, buf);
    free(buf);
    return rc;
}

static esp_err_t handle_apply_recipe(httpd_req_t *req) {
    if (!check_bearer_auth(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"unauthorized\"}");
    }
    char body[192] = {0};
    if (!recv_body_exact(req, body, sizeof(body))) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid or truncated payload");
    }

    cJSON *root = cJSON_Parse(body);
    const cJSON *jrid = root ? cJSON_GetObjectItemCaseSensitive(root, "recipe_id") : NULL;
    if (!cJSON_IsString(jrid) || jrid->valuestring[0] == '\0') {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing recipe_id");
    }

    size_t applied = 0;
    uint64_t ts = (uint64_t)(esp_timer_get_time() / 1000LL);
    if (s_cfg_mutex) xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
    edgehmi_err_t rc = (s_active_cfg && s_bus)
        ? edgehmi_recipe_apply(s_active_cfg, jrid->valuestring, s_bus, s_history, "http_api", ts, &applied)
        : EDGEHMI_ERR_IO;
    if (s_cfg_mutex) xSemaphoreGive(s_cfg_mutex);
    cJSON_Delete(root);

    if (rc != EDGEHMI_OK) {
        httpd_resp_set_status(req, "422 Unprocessable Entity");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"recipe not found or invalid\"}");
    }
    char resp[96];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"applied\":%u}", (unsigned)applied);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, resp);
}

static const char *s_setup_html =
    "<!doctype html><html><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>EdgeHMI Wi-Fi & 4G/5G Setup</title><style>body{font-family:sans-serif;background:#0b1220;color:#e2e8f0;padding:24px;max-width:440px;margin:auto}"
    "input,button{width:100%;padding:12px;margin:8px 0;border-radius:8px;border:1px solid #334155;background:#0f172a;color:#fff;font-size:15px}"
    "button{background:#0284c7;border:none;font-weight:700;cursor:pointer}</style></head><body>"
    "<h2>EdgeHMI 现场配网 (Wi-Fi / 4G / 5G)</h2><p>配置厂区 Wi-Fi 或 4G/5G 物联网卡 APN，保存至 NVS 并自动重连。</p>"
    "<input id=s placeholder='Wi-Fi SSID (可选)'><input id=p type=password placeholder='Wi-Fi Password'>"
    "<input id=a placeholder='4G/5G APN (如 cmnet / 3gnet / ctiot)'>"
    "<input id=pin type=password placeholder='工程师 PIN (默认 1234)'>"
    "<button onclick=\"fetch('/api/wifi/setup',{method:'POST',headers:{'Content-Type':'application/json'},"
    "body:JSON.stringify({ssid:s.value,password:p.value,apn:a.value,pin:pin.value})}).then(r=>r.json()).then(d=>m.textContent=d.ok?'已保存，正在接入网络...':d.error)\">保存并连接</button>"
    "<p id=m></p></body></html>";

static const char *s_h5_html =
    "<!doctype html><html><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>EdgeHMI 手机实时监控</title><style>body{font-family:sans-serif;background:#060b14;color:#e2e8f0;margin:0;padding:16px}"
    "h2{margin:0 0 10px;color:#38bdf8;font-size:18px}.card{background:#0f172a;border:1px solid #1e293b;border-radius:10px;padding:12px;margin-bottom:12px}"
    ".grid{display:grid;grid-template-columns:repeat(2,1fr);gap:8px}.t{background:#111c2e;padding:10px;border-radius:8px}"
    ".tv{font-size:20px;font-weight:700;color:#2dd4bf}.al{padding:6px 8px;border-left:3px solid #ef4444;background:#1e293b;border-radius:6px;margin-top:6px;font-size:13px}"
    "</style></head><body><h2>EdgeHMI 移动实时监控 (/h5)</h2>"
    "<div class=card id=wan>链路状态加载中...</div><div class=card><div class=grid id=tags></div></div>"
    "<div class=card><strong>实时告警</strong><div id=alms></div></div>"
    "<script>async function tick(){try{const h=await(await fetch('/api/health')).json();"
    "wan.textContent='WAN: '+(h.wan_link||'--').toUpperCase()+(h.cellular?' | '+h.cellular.rat+' '+h.cellular.rssi_dbm+'dBm':'');"
    "const t=await(await fetch('/api/tags')).json();tags.innerHTML='';"
    "(t.tags||[]).forEach(x=>{const d=document.createElement('div');d.className='t';"
    "const n=document.createElement('div');n.style.fontSize='12px';n.style.color='#94a3b8';n.textContent=x.name;"
    "const v=document.createElement('div');v.className='tv';v.textContent=Number(x.value).toFixed(2);"
    "d.appendChild(n);d.appendChild(v);tags.appendChild(d);});"
    "const a=await(await fetch('/api/alarms')).json();alms.innerHTML='';"
    "(a.alarms||[]).slice(0,6).forEach(x=>{const d=document.createElement('div');d.className='al';d.textContent=(x.ack?'[ACK] ':'[!] ')+x.message;alms.appendChild(d);});"
    "}catch(e){}}tick();setInterval(tick,1500);</script></body></html>";

static esp_err_t handle_h5_page(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_sendstr(req, s_h5_html);
}

static bool is_safe_asset_name(const char *name) {
    if (!name || name[0] == '\0' || strlen(name) > 40) return false;
    for (size_t i = 0; name[i] != '\0'; ++i) {
        char c = name[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
        if (!ok) return false;
        if (c == '.' && name[i + 1] == '.') return false;
    }
    return true;
}

static esp_err_t handle_upload_asset(httpd_req_t *req) {
    if (!check_bearer_auth(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"unauthorized\"}");
    }
    char qbuf[80] = {0};
    char fname[48] = {0};
    if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) != ESP_OK ||
        httpd_query_key_value(qbuf, "name", fname, sizeof(fname)) != ESP_OK ||
        !is_safe_asset_name(fname)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid asset name");
    }
    if (req->content_len <= 0 || req->content_len > 65536) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "asset size must be 1..65536 bytes");
    }
    uint8_t *buf = (uint8_t *)edgehmi_alloc_buf((size_t)req->content_len);
    if (!buf) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");

    int received = 0;
    while (received < req->content_len) {
        int r = httpd_req_recv(req, (char *)buf + received, req->content_len - received);
        if (r <= 0) {
            free(buf);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
        }
        received += r;
    }
    mkdir("/cfg/assets", 0755);
    char path[96];
    snprintf(path, sizeof(path), "/cfg/assets/%s", fname);
    FILE *f = fopen(path, "wb");
    if (!f) {
        free(buf);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "open asset failed");
    }
    size_t wr = fwrite(buf, 1, (size_t)received, f);
    fclose(f);
    free(buf);
    if (wr != (size_t)received) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write asset failed");
    }
    char resp[128];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"src\":\"/api/asset/%s\",\"bytes\":%u}", fname, (unsigned)wr);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, resp);
}

static esp_err_t handle_setup_page(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_sendstr(req, s_setup_html);
}

static esp_err_t handle_wifi_setup(httpd_req_t *req) {
    char body[256] = {0};
    if (!recv_body_exact(req, body, sizeof(body))) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid body");
    }

    cJSON *root = cJSON_Parse(body);
    const cJSON *jpin  = root ? cJSON_GetObjectItemCaseSensitive(root, "pin") : NULL;
    char expected_pin[16] = {0};
    if (s_cfg_mutex) xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
    if (s_active_cfg && s_active_cfg->engineer_pin[0] != '\0') {
        edgehmi_copy_str(expected_pin, sizeof(expected_pin), s_active_cfg->engineer_pin);
    }
    if (s_cfg_mutex) xSemaphoreGive(s_cfg_mutex);

    /* Require either valid Bearer token OR matching Engineer PIN to prevent rogue LAN/SoftAP hijack */
    if (expected_pin[0] != '\0' && !check_bearer_auth(req)) {
        const char *got_pin = cJSON_IsString(jpin) ? jpin->valuestring : "";
        if (!edgehmi_ct_str_eq(got_pin, expected_pin)) {
            cJSON_Delete(root);
            httpd_resp_set_status(req, "403 Forbidden");
            httpd_resp_set_type(req, "application/json");
            return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"invalid Engineer PIN\"}");
        }
    }
    const cJSON *jssid = root ? cJSON_GetObjectItemCaseSensitive(root, "ssid") : NULL;
    const cJSON *jpass = root ? cJSON_GetObjectItemCaseSensitive(root, "password") : NULL;
    const cJSON *japn  = root ? cJSON_GetObjectItemCaseSensitive(root, "apn") : NULL;
    bool updated = false;
    if (cJSON_IsString(jssid) && jssid->valuestring[0] != '\0') {
        edgehmi_net_save_wifi(jssid->valuestring, cJSON_IsString(jpass) ? jpass->valuestring : "");
        updated = true;
    }
    if (cJSON_IsString(japn) && japn->valuestring[0] != '\0') {
        edgehmi_cellular_save_apn(japn->valuestring, "auto");
        updated = true;
    }
    cJSON_Delete(root);
    if (!updated) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"missing ssid or apn\"}");
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true,\"status\":\"connecting\"}");
}

edgehmi_err_t edgehmi_http_start(edgehmi_bus_t *bus,
                                 const edgehmi_config_t *active_cfg,
                                 const edgehmi_mb_stats_t *mb_stats,
                                 edgehmi_history_t *history,
                                 SemaphoreHandle_t cfg_mutex,
                                 edgehmi_reload_cb reload_cb) {
    s_bus = bus;
    s_active_cfg = active_cfg;
    s_mb_stats = mb_stats;
    s_history = history;
    s_cfg_mutex = cfg_mutex;
    s_reload_cb = reload_cb;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.core_id = 0; /* Pin HTTP/OTA management server to Core 0 */
    config.max_uri_handlers = 22;
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        return EDGEHMI_ERR_IO;
    }

    httpd_uri_t u_health = { .uri = "/api/health",      .method = HTTP_GET,  .handler = handle_health };
    httpd_uri_t u_tags   = { .uri = "/api/tags",        .method = HTTP_GET,  .handler = handle_get_tags };
    httpd_uri_t u_cfg    = { .uri = "/api/config",      .method = HTTP_POST, .handler = handle_post_config };
    httpd_uri_t u_hist   = { .uri = "/api/history.csv", .method = HTTP_GET,  .handler = handle_history_csv };
    httpd_uri_t u_alarms = { .uri = "/api/alarms",      .method = HTTP_GET,  .handler = handle_get_alarms };
    httpd_uri_t u_ack    = { .uri = "/api/alarms/ack",  .method = HTTP_POST, .handler = handle_ack_alarms };
    httpd_uri_t u_audit  = { .uri = "/api/audit",       .method = HTTP_GET,  .handler = handle_get_audit };
    httpd_uri_t u_recipe = { .uri = "/api/recipe/apply",.method = HTTP_POST, .handler = handle_apply_recipe };
    httpd_uri_t u_asset  = { .uri = "/api/asset",       .method = HTTP_POST, .handler = handle_upload_asset };
    httpd_uri_t u_h5     = { .uri = "/h5",              .method = HTTP_GET,  .handler = handle_h5_page };
    httpd_uri_t u_setup  = { .uri = "/setup",           .method = HTTP_GET,  .handler = handle_setup_page };
    httpd_uri_t u_wifi   = { .uri = "/api/wifi/setup",  .method = HTTP_POST, .handler = handle_wifi_setup };

    httpd_register_uri_handler(server, &u_health);
    httpd_register_uri_handler(server, &u_tags);
    httpd_register_uri_handler(server, &u_cfg);
    httpd_register_uri_handler(server, &u_hist);
    httpd_register_uri_handler(server, &u_alarms);
    httpd_register_uri_handler(server, &u_ack);
    httpd_register_uri_handler(server, &u_audit);
    httpd_register_uri_handler(server, &u_recipe);
    httpd_register_uri_handler(server, &u_asset);
    httpd_register_uri_handler(server, &u_h5);
    httpd_register_uri_handler(server, &u_setup);
    httpd_register_uri_handler(server, &u_wifi);
    edgehmi_ota_register_handlers(server, active_cfg, cfg_mutex);

    ESP_LOGI(TAG, "Config + Firmware OTA + Historian + /setup Portal started on Core 0 (port %d)", config.server_port);
    return EDGEHMI_OK;
}
