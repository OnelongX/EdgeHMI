#include "modbus_tcp.h"

#include "modbus_rtu.h"

#include <math.h>
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

static size_t build_mb_exception(uint16_t tid, uint8_t uid, uint8_t fc, uint8_t ex_code,
                                 uint8_t *out, size_t cap) {
    if (!out || cap < 9) return 0;
    write_mbap(out, tid, 3, uid);
    out[7] = (uint8_t)(fc | 0x80u);
    out[8] = ex_code;
    return 9;
}

size_t edgehmi_mbtcp_handle_server_req(const uint8_t *req,
                                       size_t req_len,
                                       edgehmi_bus_t *bus,
                                       uint64_t now_ms,
                                       uint8_t *out_resp,
                                       size_t out_cap) {
    if (!req || !bus || !out_resp || req_len < 12 || out_cap < 12) {
        return 0;
    }
    uint16_t tid   = (uint16_t)(((uint16_t)req[0] << 8) | req[1]);
    uint16_t proto = (uint16_t)(((uint16_t)req[2] << 8) | req[3]);
    uint16_t mlen  = (uint16_t)(((uint16_t)req[4] << 8) | req[5]);
    uint8_t  uid   = req[6];
    uint8_t  fc    = req[7];
    if (proto != 0 || mlen < 6 || (size_t)(6 + mlen) > req_len) {
        return 0;
    }

    uint16_t addr = (uint16_t)(((uint16_t)req[8] << 8) | req[9]);
    uint16_t arg  = (uint16_t)(((uint16_t)req[10] << 8) | req[11]);

    if (fc == 1 || fc == 2) {
        if (arg == 0 || arg > 128 || (size_t)(addr + arg) > bus->count) {
            return build_mb_exception(tid, uid, fc, 0x02u, out_resp, out_cap);
        }
        uint8_t byte_cnt = (uint8_t)((arg + 7u) / 8u);
        if ((size_t)(9u + byte_cnt) > out_cap) return 0;
        write_mbap(out_resp, tid, (uint16_t)(3u + byte_cnt), uid);
        out_resp[7] = fc;
        out_resp[8] = byte_cnt;
        memset(out_resp + 9, 0, byte_cnt);
        for (uint16_t i = 0; i < arg; ++i) {
            double v = 0.0;
            edgehmi_bus_get(bus, (int16_t)(addr + i), &v, NULL);
            if (fabs(v) > 0.5) {
                out_resp[9 + (i / 8u)] |= (uint8_t)(1u << (i % 8u));
            }
        }
        return 9u + byte_cnt;
    }

    if (fc == 3 || fc == 4) {
        if (arg == 0 || arg > 64 || (size_t)(9u + arg * 2u) > out_cap) {
            return build_mb_exception(tid, uid, fc, 0x03u, out_resp, out_cap);
        }
        if (addr >= 1000u) {
            uint16_t base_idx = (uint16_t)(addr - 1000u);
            if ((size_t)(base_idx + arg) > bus->count) {
                return build_mb_exception(tid, uid, fc, 0x02u, out_resp, out_cap);
            }
            write_mbap(out_resp, tid, (uint16_t)(3u + arg * 2u), uid);
            out_resp[7] = fc;
            out_resp[8] = (uint8_t)(arg * 2u);
            for (uint16_t i = 0; i < arg; ++i) {
                double v = 0.0;
                edgehmi_bus_get(bus, (int16_t)(base_idx + i), &v, NULL);
                if (v > 32767.0) v = 32767.0;
                if (v < -32768.0) v = -32768.0;
                uint16_t raw = (uint16_t)(int16_t)lround(v);
                out_resp[9 + i * 2u]  = (uint8_t)(raw >> 8);
                out_resp[10 + i * 2u] = (uint8_t)(raw & 0xFFu);
            }
            return 9u + arg * 2u;
        }
        if ((addr % 2u) != 0u || (arg % 2u) != 0u || (size_t)((addr + arg) / 2u) > bus->count) {
            return build_mb_exception(tid, uid, fc, 0x02u, out_resp, out_cap);
        }
        write_mbap(out_resp, tid, (uint16_t)(3u + arg * 2u), uid);
        out_resp[7] = fc;
        out_resp[8] = (uint8_t)(arg * 2u);
        uint16_t tag_cnt = (uint16_t)(arg / 2u);
        uint16_t start_tag = (uint16_t)(addr / 2u);
        for (uint16_t i = 0; i < tag_cnt; ++i) {
            double v = 0.0;
            edgehmi_bus_get(bus, (int16_t)(start_tag + i), &v, NULL);
            float fv = (float)v;
            uint32_t bits = 0;
            memcpy(&bits, &fv, sizeof(bits));
            size_t off = 9u + (size_t)i * 4u;
            out_resp[off + 0] = (uint8_t)((bits >> 24) & 0xFFu);
            out_resp[off + 1] = (uint8_t)((bits >> 16) & 0xFFu);
            out_resp[off + 2] = (uint8_t)((bits >> 8) & 0xFFu);
            out_resp[off + 3] = (uint8_t)(bits & 0xFFu);
        }
        return 9u + arg * 2u;
    }

    if (fc == 5) {
        if ((size_t)addr >= bus->count) {
            return build_mb_exception(tid, uid, fc, 0x02u, out_resp, out_cap);
        }
        if (arg != 0x0000u && arg != 0xFF00u) {
            return build_mb_exception(tid, uid, fc, 0x03u, out_resp, out_cap);
        }
        if (edgehmi_bus_request_write(bus, (int16_t)addr, arg ? 1.0 : 0.0, now_ms) != EDGEHMI_OK) {
            return build_mb_exception(tid, uid, fc, 0x04u, out_resp, out_cap);
        }
        memcpy(out_resp, req, 12);
        return 12;
    }

    if (fc == 6) {
        if (addr < 1000u || (size_t)(addr - 1000u) >= bus->count) {
            return build_mb_exception(tid, uid, fc, 0x02u, out_resp, out_cap);
        }
        int16_t sv = (int16_t)arg;
        if (edgehmi_bus_request_write(bus, (int16_t)(addr - 1000u), (double)sv, now_ms) != EDGEHMI_OK) {
            return build_mb_exception(tid, uid, fc, 0x04u, out_resp, out_cap);
        }
        memcpy(out_resp, req, 12);
        return 12;
    }

    if (fc == 16) {
        if (req_len < 17 || arg != 2u || req[12] != 4u || (addr % 2u) != 0u || (size_t)(addr / 2u) >= bus->count) {
            return build_mb_exception(tid, uid, fc, 0x02u, out_resp, out_cap);
        }
        uint32_t bits = ((uint32_t)req[13] << 24) | ((uint32_t)req[14] << 16) |
                        ((uint32_t)req[15] << 8)  | (uint32_t)req[16];
        float fv = 0.0f;
        memcpy(&fv, &bits, sizeof(fv));
        if (!isfinite(fv) || edgehmi_bus_request_write(bus, (int16_t)(addr / 2u), (double)fv, now_ms) != EDGEHMI_OK) {
            return build_mb_exception(tid, uid, fc, 0x04u, out_resp, out_cap);
        }
        write_mbap(out_resp, tid, 6, uid);
        out_resp[7]  = 16;
        out_resp[8]  = req[8];
        out_resp[9]  = req[9];
        out_resp[10] = 0;
        out_resp[11] = 2;
        return 12;
    }

    return build_mb_exception(tid, uid, fc, 0x01u, out_resp, out_cap);
}
