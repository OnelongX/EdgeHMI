#ifndef EDGEHMI_DISCOVERY_PROTO_H
#define EDGEHMI_DISCOVERY_PROTO_H

#include "edgehmi_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EDGEHMI_DISCOVERY_UDP_PORT 56888

typedef struct {
    char device_id[32];
    char name[48];
    char fw_version[16];
    char cfg_version[16];
    uint16_t http_port;
    uint16_t width;
    uint16_t height;
    bool comm_loss;
    uint32_t uptime_s;
} edgehmi_device_info_t;

/* Returns true if UDP payload is a valid EdgeHMI discovery probe */
bool edgehmi_discovery_is_probe(const char *msg, size_t len);

/* Format JSON discovery announcement packet for UDP reply */
size_t edgehmi_discovery_build_announce(const edgehmi_device_info_t *info,
                                        char *out_buf,
                                        size_t out_cap);

#ifdef __cplusplus
}
#endif

#endif /* EDGEHMI_DISCOVERY_PROTO_H */
