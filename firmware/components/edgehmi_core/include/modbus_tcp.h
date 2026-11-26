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

/*
 * Northbound Modbus TCP Server Gateway (SCADA / PLC protocol bridge):
 *   - Coils / Discrete Inputs (FC01/FC02): addr i -> tag[i] boolean state (|val| > 0.5)
 *   - Holding / Input Registers (FC03/FC04):
 *       * addr 2*i .. 2*i+1 (0..255): IEEE-754 float32 (ABCD big-endian) of tag[i]
 *       * addr 1000+i (1000..1127): signed int16 rounded engineering value of tag[i]
 *   - Remote Writes:
 *       * FC05 single coil i -> request_write(tag[i], 0.0 / 1.0)
 *       * FC06 single register 1000+i -> request_write(tag[i], (int16_t)raw)
 *       * FC16 two registers at 2*i -> IEEE-754 float32 request_write(tag[i], fval)
 * Returns response frame length (>0) written to out_resp, or 0 on malformed MBAP header.
 */
size_t edgehmi_mbtcp_handle_server_req(const uint8_t *req,
                                       size_t req_len,
                                       edgehmi_bus_t *bus,
                                       uint64_t now_ms,
                                       uint8_t *out_resp,
                                       size_t out_cap);

#ifdef __cplusplus
}
#endif

#endif /* EDGEHMI_MODBUS_TCP_H */
