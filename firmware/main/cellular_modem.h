#ifndef EDGEHMI_CELLULAR_MODEM_H
#define EDGEHMI_CELLULAR_MODEM_H

#include "cellular_proto.h"
#include "edgehmi_types.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Start Core 0 4G Cat.1/Cat.4 & 5G RedCap/NR modem manager + Wi-Fi/Cellular WAN failover */
edgehmi_err_t edgehmi_cellular_start(const edgehmi_config_t *active_cfg,
                                     esp_netif_t *wifi_sta_netif,
                                     SemaphoreHandle_t cfg_mutex);

/* Update Wi-Fi STA online flag (called from Wi-Fi IP event handler) */
void edgehmi_cellular_notify_wifi(bool wifi_has_ip);

/* Thread-safe snapshot of live 4G/5G signal, RAT, operator, ICCID & active WAN link */
void edgehmi_cellular_get_status(edgehmi_cell_status_t *out_cell,
                                 edgehmi_wan_link_t *out_active_link);

/* Persist APN & 4G/5G mode override in NVS (used by /setup field portal) */
edgehmi_err_t edgehmi_cellular_save_apn(const char *apn, const char *generation);

#ifdef __cplusplus
}
#endif

#endif /* EDGEHMI_CELLULAR_MODEM_H */
