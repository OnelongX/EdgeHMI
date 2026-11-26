#ifndef EDGEHMI_OTA_CONFIG_H
#define EDGEHMI_OTA_CONFIG_H

#include "data_bus.h"
#include "edgehmi_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "history_log.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EDGEHMI_FS_BASE        "/cfg"
#define EDGEHMI_ACTIVE_PATH    "/cfg/active.json"
#define EDGEHMI_STAGING_PATH   "/cfg/staging.json"
#define EDGEHMI_BACKUP_PATH    "/cfg/backup.json"

typedef edgehmi_err_t (*edgehmi_reload_cb)(const char *json_buf, char *err_buf, size_t err_len);

/* PSRAM-preferred allocation helper with automatic fallback to internal heap */
void *edgehmi_alloc_buf(size_t size);

/* Mount LittleFS partition */
edgehmi_err_t edgehmi_storage_init(void);

/* Boot-OK watchdog rollback guard: auto-restores backup.json after >=2 consecutive crash boots */
bool edgehmi_boot_begin_check(void);
void edgehmi_boot_mark_healthy(void);

/* Read active (or backup) config JSON from LittleFS into caller buffer */
edgehmi_err_t edgehmi_storage_read_active(char *out_buf, size_t cap, size_t *out_len);

/* Atomically persist validated JSON config (staging -> backup -> active) */
edgehmi_err_t edgehmi_storage_save_active(const char *json_buf, size_t len);

/* Trigger deferred UI rebuild on Core 1 (safe to call from Core 0 HTTP/asset handlers) */
void edgehmi_request_ui_reload(void);

/* Start HTTP server exposing /api/health, /api/tags, /api/config, /api/history.csv, /api/alarms */
edgehmi_err_t edgehmi_http_start(edgehmi_bus_t *bus,
                                 const edgehmi_config_t *active_cfg,
                                 const edgehmi_mb_stats_t *mb_stats,
                                 edgehmi_history_t *history,
                                 SemaphoreHandle_t cfg_mutex,
                                 edgehmi_reload_cb reload_cb);

#ifdef __cplusplus
}
#endif

#endif /* EDGEHMI_OTA_CONFIG_H */
