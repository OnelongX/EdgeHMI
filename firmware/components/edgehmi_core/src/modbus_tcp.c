#include "modbus_tcp.h"

#include "modbus_rtu.h"

#include <string.h>

static void write_mbap(uint8_t *buf, uint16_t trans_id, uint16_t pdu_plus_uid_len, uint8_t unit_id) {
    buf[0] = (uint8_t)(trans_id >> 8);
    buf[1] = (uint8_t)(trans_id & 0xFFu);
    buf[2] = 0x00u; /* Protocol ID high = 0 (Modbus) */
    buf[3] = 0x00u; /* Protocol ID low  = 0 */
    buf[4] = (uint8_t)(pdu_plus_uid_len >> 8);
    buf[5] = (uint8_t)(pdu_plus_uid_len & 0xFFu);
    buf[6] = unit_id;
}

size_t edgehmi_mbtcp_build_read_req(uint16_t trans_id,
                                    uint8_t unit_id,
                                    uint8_t func_code,
                                    uint16_t start_addr,
                                    uint16_t quantity,
                                    uint8_t *out_buf,
                                    size_t out_cap) {
    if (!out_buf || out_cap < 12) {
        return 0;
    }
    write_mbap(out_buf, trans_id, 6, unit_id);
    out_buf[7]  = func_code;
    out_buf[8]  = (uint8_t)(start_addr >> 8);
    out_buf[9]  = (uint8_t)(start_addr & 0xFFu);
    out_buf[10] = (uint8_t)(quantity >> 8);
    out_buf[11] = (uint8_t)(quantity & 0xFFu);
    return 12;
}

size_t edgehmi_mbtcp_build_write_single(uint16_t trans_id,
                                        uint8_t unit_id,
                                        uint8_t func_code,
                                        uint16_t reg_addr,
                                        uint16_t raw_val,
                                        uint8_t *out_buf,
                                        size_t out_cap) {
    if (!out_buf || out_cap < 12) {
        return 0;
    }
    uint16_t wire = raw_val;
    if (func_code == 5) {
        wire = raw_val ? 0xFF00u : 0x0000u;
    }
    write_mbap(out_buf, trans_id, 6, unit_id);
    out_buf[7]  = func_code;
    out_buf[8]  = (uint8_t)(reg_addr >> 8);
    out_buf[9]  = (uint8_t)(reg_addr & 0xFFu);
    out_buf[10] = (uint8_t)(wire >> 8);
    out_buf[11] = (uint8_t)(wire & 0xFFu);
    return 12;
}

size_t edgehmi_mbtcp_build_write_multi(uint16_t trans_id,
                                       const edgehmi_mb_point_t *pt,
                                       double eng_val,
                                       uint8_t *out_buf,
                                       size_t out_cap) {
    if (!pt || !out_buf || out_cap < 17) {
        return 0;
    }
    /* Reuse RTU FC16 encoder for exact 4-byte order serialization (ABCD/CDAB/BADC/DCBA) */
    uint8_t rtu[16];
    size_t rlen = edgehmi_mb_build_write_multi(pt, eng_val, rtu, sizeof(rtu));
    if (rlen < 13) {
        return 0;
    }
    size_t body_len = rlen - 2; /* Strip 2-byte RTU CRC; rtu[0] is unit_id */
    if (6 + body_len > out_cap) {
        return 0;
    }
    write_mbap(out_buf, trans_id, (uint16_t)body_len, rtu[0]);
    memcpy(out_buf + 7, rtu + 1, body_len - 1);
    return 6 + body_len;
}

edgehmi_err_t edgehmi_mbtcp_parse_block_resp(uint16_t expected_trans_id,
                                             const edgehmi_mb_block_t *blk,
                                             const edgehmi_mb_point_t *points,
                                             const uint8_t *frame,
                                             size_t frame_len,
                                             edgehmi_bus_t *bus,
                                             uint64_t now_ms) {
    if (!blk || !points || !frame || !bus || frame_len < 9) {
        return EDGEHMI_ERR_INVALID_ARG;
    }
    uint16_t rx_tid = (uint16_t)(((uint16_t)frame[0] << 8) | frame[1]);
    uint16_t proto  = (uint16_t)(((uint16_t)frame[2] << 8) | frame[3]);
    uint16_t mlen   = (uint16_t)(((uint16_t)frame[4] << 8) | frame[5]);
    if (rx_tid != expected_trans_id || proto != 0) {
        return EDGEHMI_ERR_PARSE;
    }
    if (mlen < 3 || (size_t)(6 + mlen) > frame_len || (size_t)mlen > 256) {
        return EDGEHMI_ERR_PARSE;
    }

    /* Synthesize RTU frame (unit_id + PDU + CRC16) to reuse 100% of RTU block unpacking */
    uint8_t rtu_buf[260];
    memcpy(rtu_buf, frame + 6, mlen);
    uint16_t crc = edgehmi_mb_crc16(rtu_buf, mlen);
    rtu_buf[mlen]     = (uint8_t)(crc & 0xFFu);
    rtu_buf[mlen + 1] = (uint8_t)(crc >> 8);
    return edgehmi_mb_parse_block_resp(blk, points, rtu_buf, (size_t)mlen + 2, bus, now_ms);
}
