#ifndef EDGEHMI_CAN_CODEC_H
#define EDGEHMI_CAN_CODEC_H

#include "data_bus.h"
#include "edgehmi_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Extract a bitfield signal (1..32 bits, Intel LE or Motorola BE, signed/unsigned) from a CAN frame */
edgehmi_err_t edgehmi_can_decode_signal(const edgehmi_can_signal_t *sig,
                                        const uint8_t *data,
                                        uint8_t dlc,
                                        double *out_eng_val);

/* Decode all matching signals in a received CAN 2.0B frame directly into edgehmi_bus_t */
size_t edgehmi_can_ingest_frame(const edgehmi_can_cfg_t *can_cfg,
                                uint32_t can_id,
                                bool ext_id,
                                const uint8_t *data,
                                uint8_t dlc,
                                edgehmi_bus_t *bus,
                                uint64_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* EDGEHMI_CAN_CODEC_H */
