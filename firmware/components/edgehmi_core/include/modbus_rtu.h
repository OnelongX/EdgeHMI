#ifndef EDGEHMI_MODBUS_RTU_H
#define EDGEHMI_MODBUS_RTU_H

#include "data_bus.h"
#include "edgehmi_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Standard Modbus RTU CRC-16 (polynomial 0xA001) */
uint16_t edgehmi_mb_crc16(const uint8_t *buf, size_t len);

/* Build Read Coils (FC01), Read Discrete (FC02), Read Holding (FC03), Read Input (FC04) */
size_t edgehmi_mb_build_read_req(uint8_t slave_id,
                                 uint8_t func_code,
                                 uint16_t start_addr,
                                 uint16_t quantity,
                                 uint8_t *out_buf,
                                 size_t out_cap);

/* Build Write Single Coil (FC05) or Write Single Register (FC06) */
size_t edgehmi_mb_build_write_single(uint8_t slave_id,
                                     uint8_t func_code,
                                     uint16_t reg_addr,
                                     uint16_t raw_val,
                                     uint8_t *out_buf,
                                     size_t out_cap);

/* Build Write Multiple Registers (FC16) for 32-bit u32/f32 with configured byte order */
size_t edgehmi_mb_build_write_multi(const edgehmi_mb_point_t *pt,
                                    double eng_val,
                                    uint8_t *out_buf,
                                    size_t out_cap);

/* Verify CRC & parse a read response for a single point, applying scale & offset */
edgehmi_err_t edgehmi_mb_parse_read_resp(const edgehmi_mb_point_t *pt,
                                         const uint8_t *frame,
                                         size_t frame_len,
                                         double *out_eng_val);

/* Convert engineering value to raw 16-bit Modbus register/coil value */
uint16_t edgehmi_mb_eng_to_raw16(const edgehmi_mb_point_t *pt, double eng_val);

/* Coalesce adjacent readable points into contiguous Modbus Block Read requests */
size_t edgehmi_mb_plan_blocks(const edgehmi_mb_point_t *points,
                              size_t point_count,
                              edgehmi_mb_block_t *out_blocks,
                              size_t max_blocks,
                              uint16_t max_gap,
                              uint16_t max_regs);

/* Verify CRC once and unpack all points in a block read response directly into data_bus */
edgehmi_err_t edgehmi_mb_parse_block_resp(const edgehmi_mb_block_t *blk,
                                          const edgehmi_mb_point_t *points,
                                          const uint8_t *frame,
                                          size_t frame_len,
                                          edgehmi_bus_t *bus,
                                          uint64_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* EDGEHMI_MODBUS_RTU_H */
