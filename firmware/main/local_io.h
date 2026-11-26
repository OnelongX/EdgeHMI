#ifndef EDGEHMI_LOCAL_IO_H
#define EDGEHMI_LOCAL_IO_H

#include "data_bus.h"
#include "edgehmi_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Configure ESP32-S3 native GPIO DI/DO, ADC1 and LEDC PWM pins from JSON local_io[] */
edgehmi_err_t edgehmi_local_io_configure(const edgehmi_config_t *cfg);

/* Bidirectional Core 1 tick: flush DO/PWM writes and sample DI/ADC into data_bus */
void edgehmi_local_io_sync(const edgehmi_config_t *cfg, edgehmi_bus_t *bus, uint64_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* EDGEHMI_LOCAL_IO_H */
