#ifndef EDGEHMI_OTA_FIRMWARE_H
#define EDGEHMI_OTA_FIRMWARE_H

#include "edgehmi_types.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "ota_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EDGEHMI_FW_VERSION "1.1.0"

/* Register POST /api/ota/firmware (Push) and POST /api/ota/pull (Pull) on HTTP server */
edgehmi_err_t edgehmi_ota_register_handlers(httpd_handle_t server,
                                            const edgehmi_config_t *active_cfg,
                                            SemaphoreHandle_t cfg_mutex);

/* Confirm running OTA image is healthy to cancel ESP-IDF automatic bootloader rollback */
void edgehmi_ota_mark_app_valid(void);

/* Check and apply an offline .epk package (e.g. /sdcard/update.epk) */
edgehmi_err_t edgehmi_ota_apply_offline_epk(const char *epk_path,
                                            edgehmi_reload_cb reload_cb);

#ifdef __cplusplus
}
#endif

#endif /* EDGEHMI_OTA_FIRMWARE_H */
