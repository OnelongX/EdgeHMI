#ifndef EDGEHMI_MODBUS_TCP_H
#define EDGEHMI_MODBUS_TCP_H

#include "data_bus.h"
#include "edgehmi_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EDGEHMI_MB_TCP_MBAP_LEN 7

/* Build Modbus TCP Read Request (MBAP 7B + PDU 5B = 12 bytes) */
size_t edgehmi_mbtcp_build_read_req(uint16_t trans_id,
                                    uint8_t unit_id,
                                    uint8_t func_code,
                                    uint16_t start_addr,
                                    uint16_t quantity,
                                    uint8_t *out_buf,
                                    size_t out_cap);

/* Build Modbus TCP Write Single Coil/Register (FC05/FC06, 12 bytes) */
size_t edgehmi_mbtcp_build_write_single(uint16_t trans_id,
                                        uint8_t unit_id,
                                        uint8_t func_code,
                                        uint16_t reg_addr,
                                        uint16_t raw_val,
                                        uint8_t *out_buf,
                                        size_t out_cap);

/* Build Modbus TCP Write Multiple Registers (FC16) for 32-bit u32/f32 */
size_t edgehmi_mbtcp_build_write_multi(uint16_t trans_id,
                                       const edgehmi_mb_point_t *pt,
                                       double eng_val,
                                       uint8_t *out_buf,
                                       size_t out_cap);

/* Verify MBAP header & unpack all points in a Modbus TCP block read response into data_bus */
edgehmi_err_t edgehmi_mbtcp_parse_block_resp(uint16_t expected_trans_id,
                                             const edgehmi_mb_block_t *blk,
                                             const edgehmi_mb_point_t *points,
                                             const uint8_t *frame,
                                             size_t frame_len,
                                             edgehmi_bus_t *bus,
                                             uint64_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* EDGEHMI_MODBUS_TCP_H */
