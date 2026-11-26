#ifndef EDGEHMI_CLOUD_MQTT_H
#define EDGEHMI_CLOUD_MQTT_H

#include "data_bus.h"
#include "edgehmi_types.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "history_log.h"
#include "rule_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize Wi-Fi STA (with SoftAP fallback) and return STA netif for multi-link 4G/5G failover */
esp_netif_t *edgehmi_net_start(const edgehmi_config_t *cfg);

/* Start Core 0 MQTT telemetry publisher + remote command subscriber with offline ring-buffer */
edgehmi_err_t edgehmi_cloud_mqtt_start(const edgehmi_config_t *active_cfg,
                                       edgehmi_bus_t *bus,
                                       edgehmi_history_t *history,
                                       SemaphoreHandle_t cfg_mutex);

/* Reconnect MQTT client after config hot-reload changes broker URI/credentials */
void edgehmi_cloud_mqtt_reconfigure(void);

/* Persist Wi-Fi STA credentials to NVS and trigger immediate reconnect */
edgehmi_err_t edgehmi_net_save_wifi(const char *ssid, const char *password);

/* Push an immediate alarm event to cloud MQTT (or queue into offline ring-buffer) */
void edgehmi_cloud_mqtt_publish_alarm(const edgehmi_alarm_event_t *evt);

#ifdef __cplusplus
}
#endif

#endif /* EDGEHMI_CLOUD_MQTT_H */
