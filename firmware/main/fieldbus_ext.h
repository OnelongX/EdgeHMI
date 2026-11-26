#ifndef EDGEHMI_FIELDBUS_EXT_H
#define EDGEHMI_FIELDBUS_EXT_H

#include "data_bus.h"
#include "edgehmi_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize / reconfigure ESP32-S3 native TWAI (CAN 2.0B) & Modbus TCP client */
edgehmi_err_t edgehmi_fieldbus_ext_configure(const edgehmi_config_t *cfg);

/* Drain pending CAN 2.0B frames into data_bus (non-blocking, called from Core 1 control_task) */
size_t edgehmi_can_poll_rx(const edgehmi_config_t *cfg, edgehmi_bus_t *bus, uint64_t now_ms);

/* Execute one Modbus TCP block scan cycle if cfg->modbus_tcp.enabled is true */
void edgehmi_mbtcp_scan_blocks(const edgehmi_config_t *cfg,
                               const edgehmi_mb_block_t *blocks,
                               size_t block_count,
                               edgehmi_bus_t *bus,
                               edgehmi_mb_stats_t *stats,
                               uint64_t now_ms);

/* Start Core 0 Northbound Modbus TCP Server Gateway for external SCADA / PLC polling */
edgehmi_err_t edgehmi_mbtcp_server_start(const edgehmi_config_t *active_cfg,
                                         edgehmi_bus_t *bus,
                                         SemaphoreHandle_t cfg_mutex);

#ifdef __cplusplus
}
#endif

#endif /* EDGEHMI_FIELDBUS_EXT_H */
