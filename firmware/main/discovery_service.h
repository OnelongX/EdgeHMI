#ifndef EDGEHMI_DISCOVERY_SERVICE_H
#define EDGEHMI_DISCOVERY_SERVICE_H

#include "data_bus.h"
#include "edgehmi_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Start mDNS (_edgehmi._tcp) and UDP 56888 broadcast discovery responder on Core 0 */
edgehmi_err_t edgehmi_discovery_start(const edgehmi_config_t *active_cfg,
                                      edgehmi_bus_t *bus,
                                      SemaphoreHandle_t cfg_mutex,
                                      uint16_t http_port);

#ifdef __cplusplus
}
#endif

#endif /* EDGEHMI_DISCOVERY_SERVICE_H */
