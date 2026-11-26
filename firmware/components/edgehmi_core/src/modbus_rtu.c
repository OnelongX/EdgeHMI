#include "modbus_rtu.h"

#include <math.h>
#include <string.h>

static uint32_t decode_u32_order(const uint8_t *b, edgehmi_byte_order_t order) {
    switch (order) {
        case BYTE_ORDER_CDAB:
            return ((uint32_t)b[2] << 24) | ((uint32_t)b[3] << 16) |
                   ((uint32_t)b[0] << 8)  | (uint32_t)b[1];
        case BYTE_ORDER_BADC:
            return ((uint32_t)b[1] << 24) | ((uint32_t)b[0] << 16) |
                   ((uint32_t)b[3] << 8)  | (uint32_t)b[2];
        case BYTE_ORDER_DCBA:
            return ((uint32_t)b[3] << 24) | ((uint32_t)b[2] << 16) |
                   ((uint32_t)b[1] << 8)  | (uint32_t)b[0];
        case BYTE_ORDER_ABCD:
        default:
            return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
                   ((uint32_t)b[2] << 8)  | (uint32_t)b[3];
    }
}

static void encode_u32_order(uint32_t v, edgehmi_byte_order_t order, uint8_t out[4]) {
    uint8_t a = (uint8_t)((v >> 24) & 0xFFu);
    uint8_t b = (uint8_t)((v >> 16) & 0xFFu);
    uint8_t c = (uint8_t)((v >> 8) & 0xFFu);
    uint8_t d = (uint8_t)(v & 0xFFu);
    switch (order) {
        case BYTE_ORDER_CDAB: out[0] = c; out[1] = d; out[2] = a; out[3] = b; break;
        case BYTE_ORDER_BADC: out[0] = b; out[1] = a; out[2] = d; out[3] = c; break;
        case BYTE_ORDER_DCBA: out[0] = d; out[1] = c; out[2] = b; out[3] = a; break;
        case BYTE_ORDER_ABCD:
        default:              out[0] = a; out[1] = b; out[2] = c; out[3] = d; break;
    }
}

uint16_t edgehmi_mb_crc16(const uint8_t *buf, size_t len) {
    uint16_t crc = 0xFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= (uint16_t)buf[i];
        for (int b = 0; b < 8; ++b) {
            if (crc & 0x0001u) {
                crc = (uint16_t)((crc >> 1) ^ 0xA001u);
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

size_t edgehmi_mb_build_read_req(uint8_t slave_id,
                                 uint8_t func_code,
                                 uint16_t start_addr,
                                 uint16_t quantity,
                                 uint8_t *out_buf,
                                 size_t out_cap) {
    if (!out_buf || out_cap < 8) {
        return 0;
    }
    out_buf[0] = slave_id;
    out_buf[1] = func_code;
    out_buf[2] = (uint8_t)(start_addr >> 8);
    out_buf[3] = (uint8_t)(start_addr & 0xFFu);
    out_buf[4] = (uint8_t)(quantity >> 8);
    out_buf[5] = (uint8_t)(quantity & 0xFFu);
    uint16_t crc = edgehmi_mb_crc16(out_buf, 6);
    out_buf[6] = (uint8_t)(crc & 0xFFu);        /* CRC Lo */
    out_buf[7] = (uint8_t)((crc >> 8) & 0xFFu); /* CRC Hi */
    return 8;
}

size_t edgehmi_mb_build_write_single(uint8_t slave_id,
                                     uint8_t func_code,
                                     uint16_t reg_addr,
                                     uint16_t raw_val,
                                     uint8_t *out_buf,
                                     size_t out_cap) {
    if (!out_buf || out_cap < 8) {
        return 0;
    }
    uint16_t payload = raw_val;
    if (func_code == 5) {
        payload = (raw_val != 0) ? 0xFF00u : 0x0000u;
    }
    out_buf[0] = slave_id;
    out_buf[1] = func_code;
    out_buf[2] = (uint8_t)(reg_addr >> 8);
    out_buf[3] = (uint8_t)(reg_addr & 0xFFu);
    out_buf[4] = (uint8_t)(payload >> 8);
    out_buf[5] = (uint8_t)(payload & 0xFFu);
    uint16_t crc = edgehmi_mb_crc16(out_buf, 6);
    out_buf[6] = (uint8_t)(crc & 0xFFu);
    out_buf[7] = (uint8_t)((crc >> 8) & 0xFFu);
    return 8;
}

size_t edgehmi_mb_build_write_multi(const edgehmi_mb_point_t *pt,
                                    double eng_val,
                                    uint8_t *out_buf,
                                    size_t out_cap) {
    if (!pt || !out_buf || out_cap < 13) {
        return 0;
    }
    double scale = (fabs(pt->scale) < 1e-12) ? 1.0 : pt->scale;
    double raw = (eng_val - pt->offset) / scale;
    uint32_t bits = 0;

    if (pt->type == TAG_TYPE_F32) {
        float f = (float)raw;
        memcpy(&bits, &f, sizeof(bits));
    } else {
        if (raw < 0.0) raw = 0.0;
        if (raw > 4294967295.0) raw = 4294967295.0;
        bits = (uint32_t)llround(raw);
    }

    uint8_t payload[4];
    encode_u32_order(bits, pt->byte_order, payload);

    out_buf[0] = pt->slave_id;
    out_buf[1] = 16; /* FC16 Write Multiple Registers */
    out_buf[2] = (uint8_t)(pt->reg_addr >> 8);
    out_buf[3] = (uint8_t)(pt->reg_addr & 0xFFu);
    out_buf[4] = 0x00u;
    out_buf[5] = 0x02u; /* 2 registers = 32 bits */
    out_buf[6] = 0x04u; /* 4 data bytes */
    memcpy(&out_buf[7], payload, 4);
    uint16_t crc = edgehmi_mb_crc16(out_buf, 11);
    out_buf[11] = (uint8_t)(crc & 0xFFu);
    out_buf[12] = (uint8_t)((crc >> 8) & 0xFFu);
    return 13;
}

edgehmi_err_t edgehmi_mb_parse_read_resp(const edgehmi_mb_point_t *pt,
                                         const uint8_t *frame,
                                         size_t frame_len,
                                         double *out_eng_val) {
    if (!pt || !frame || !out_eng_val || frame_len < 5) {
        return EDGEHMI_ERR_INVALID_ARG;
    }

    uint16_t expected_crc = edgehmi_mb_crc16(frame, frame_len - 2);
    uint16_t got_crc = (uint16_t)frame[frame_len - 2] |
                       ((uint16_t)frame[frame_len - 1] << 8);
    if (expected_crc != got_crc) {
        return EDGEHMI_ERR_CRC;
    }

    if (frame[0] != pt->slave_id) {
        return EDGEHMI_ERR_PARSE;
    }
    if ((frame[1] & 0x80u) != 0) {
        return EDGEHMI_ERR_IO; /* Modbus exception response */
    }
    if (frame[1] != pt->func_code) {
        return EDGEHMI_ERR_PARSE;
    }

    uint8_t byte_count = frame[2];
    if ((size_t)(3 + byte_count + 2) > frame_len) {
        return EDGEHMI_ERR_PARSE;
    }

    const uint8_t *data = &frame[3];
    double raw = 0.0;

    switch (pt->type) {
        case TAG_TYPE_BOOL:
            if (byte_count < 1) {
                return EDGEHMI_ERR_PARSE;
            }
            if (pt->func_code == 1 || pt->func_code == 2) {
                raw = (double)(data[0] & 0x01u);
            } else {
                if (byte_count < 2) {
                    return EDGEHMI_ERR_PARSE;
                }
                uint16_t v = ((uint16_t)data[0] << 8) | data[1];
                raw = (v != 0) ? 1.0 : 0.0;
            }
            break;

        case TAG_TYPE_U16: {
            if (byte_count < 2) {
                return EDGEHMI_ERR_PARSE;
            }
            uint16_t v = ((uint16_t)data[0] << 8) | data[1];
            raw = (double)v;
            break;
        }

        case TAG_TYPE_I16: {
            if (byte_count < 2) {
                return EDGEHMI_ERR_PARSE;
            }
            uint16_t u = ((uint16_t)data[0] << 8) | data[1];
            int16_t s = 0;
            memcpy(&s, &u, sizeof(s));
            raw = (double)s;
            break;
        }

        case TAG_TYPE_U32: {
            if (byte_count < 4) {
                return EDGEHMI_ERR_PARSE;
            }
            uint32_t v = decode_u32_order(data, pt->byte_order);
            raw = (double)v;
            break;
        }

        case TAG_TYPE_F32: {
            if (byte_count < 4) {
                return EDGEHMI_ERR_PARSE;
            }
            uint32_t bits = decode_u32_order(data, pt->byte_order);
            float f = 0.0f;
            memcpy(&f, &bits, sizeof(f));
            raw = (double)f;
            break;
        }

        default:
            return EDGEHMI_ERR_INVALID_ARG;
    }

    double scale = (fabs(pt->scale) < 1e-12) ? 1.0 : pt->scale;
    *out_eng_val = raw * scale + pt->offset;
    return EDGEHMI_OK;
}

uint16_t edgehmi_mb_eng_to_raw16(const edgehmi_mb_point_t *pt, double eng_val) {
    if (!pt) {
        return 0;
    }
    if (pt->type == TAG_TYPE_BOOL) {
        return (fabs(eng_val) > 0.5) ? 1u : 0u;
    }
    double scale = (fabs(pt->scale) < 1e-12) ? 1.0 : pt->scale;
    double raw = (eng_val - pt->offset) / scale;
    if (pt->type == TAG_TYPE_I16) {
        long rounded = lround(raw);
        if (rounded < -32768L) rounded = -32768L;
        if (rounded > 32767L) rounded = 32767L;
        int16_t s = (int16_t)rounded;
        uint16_t u = 0;
        memcpy(&u, &s, sizeof(u));
        return u;
    }
    long rounded = lround(raw);
    if (rounded < 0L) rounded = 0L;
    if (rounded > 65535L) rounded = 65535L;
    return (uint16_t)rounded;
}

static uint16_t point_reg_span(const edgehmi_mb_point_t *pt) {
    if (pt->func_code == 1 || pt->func_code == 2) {
        return 1;
    }
    return (pt->type == TAG_TYPE_U32 || pt->type == TAG_TYPE_F32) ? 2 : 1;
}

static bool point_less(const edgehmi_mb_point_t *a, const edgehmi_mb_point_t *b) {
    if (a->slave_id != b->slave_id) return a->slave_id < b->slave_id;
    if (a->func_code != b->func_code) return a->func_code < b->func_code;
    return a->reg_addr < b->reg_addr;
}

size_t edgehmi_mb_plan_blocks(const edgehmi_mb_point_t *points,
                              size_t point_count,
                              edgehmi_mb_block_t *out_blocks,
                              size_t max_blocks,
                              uint16_t max_gap,
                              uint16_t max_regs) {
    if (!points || !out_blocks || max_blocks == 0 || point_count == 0) {
        return 0;
    }
    if (max_regs == 0) {
        max_regs = 32;
    }

    uint16_t sorted[EDGEHMI_MAX_MODBUS_POINTS];
    size_t readable = 0;
    for (size_t i = 0; i < point_count && readable < EDGEHMI_MAX_MODBUS_POINTS; ++i) {
        const edgehmi_mb_point_t *p = &points[i];
        if ((p->access & TAG_ACCESS_R) == 0) continue;
        if (p->func_code < 1 || p->func_code > 4) continue;
        sorted[readable++] = (uint16_t)i;
    }

    /* Deterministic insertion sort by (slave_id, func_code, reg_addr) */
    for (size_t i = 1; i < readable; ++i) {
        uint16_t key = sorted[i];
        size_t j = i;
        while (j > 0 && point_less(&points[key], &points[sorted[j - 1]])) {
            sorted[j] = sorted[j - 1];
            j--;
        }
        sorted[j] = key;
    }

    size_t block_count = 0;
    for (size_t i = 0; i < readable; ++i) {
        uint16_t idx = sorted[i];
        const edgehmi_mb_point_t *p = &points[idx];
        uint16_t span = point_reg_span(p);

        bool merged = false;
        if (block_count > 0) {
            edgehmi_mb_block_t *blk = &out_blocks[block_count - 1];
            uint32_t blk_end = (uint32_t)blk->start_addr + (uint32_t)blk->quantity;
            uint32_t pt_end = (uint32_t)p->reg_addr + (uint32_t)span;
            uint32_t new_end = (pt_end > blk_end) ? pt_end : blk_end;
            if (blk->slave_id == p->slave_id &&
                blk->func_code == p->func_code &&
                blk->point_count < 32 &&
                (uint32_t)p->reg_addr <= blk_end + (uint32_t)max_gap &&
                (new_end - (uint32_t)blk->start_addr) <= (uint32_t)max_regs) {
                blk->quantity = (uint16_t)(new_end - (uint32_t)blk->start_addr);
                blk->point_indices[blk->point_count++] = idx;
                merged = true;
            }
        }

        if (!merged) {
            if (block_count >= max_blocks) {
                break;
            }
            edgehmi_mb_block_t *blk = &out_blocks[block_count++];
            memset(blk, 0, sizeof(*blk));
            blk->slave_id = p->slave_id;
            blk->func_code = p->func_code;
            blk->start_addr = p->reg_addr;
            blk->quantity = span;
            blk->point_indices[0] = idx;
            blk->point_count = 1;
        }
    }

    return block_count;
}

edgehmi_err_t edgehmi_mb_parse_block_resp(const edgehmi_mb_block_t *blk,
                                          const edgehmi_mb_point_t *points,
                                          const uint8_t *frame,
                                          size_t frame_len,
                                          edgehmi_bus_t *bus,
                                          uint64_t now_ms) {
    if (!blk || !points || !frame || !bus || frame_len < 5) {
        return EDGEHMI_ERR_INVALID_ARG;
    }

    uint16_t expected_crc = edgehmi_mb_crc16(frame, frame_len - 2);
    uint16_t got_crc = (uint16_t)frame[frame_len - 2] |
                       ((uint16_t)frame[frame_len - 1] << 8);
    if (expected_crc != got_crc) {
        return EDGEHMI_ERR_CRC;
    }
    if (frame[0] != blk->slave_id) {
        return EDGEHMI_ERR_PARSE;
    }
    if ((frame[1] & 0x80u) != 0) {
        return EDGEHMI_ERR_IO;
    }
    if (frame[1] != blk->func_code) {
        return EDGEHMI_ERR_PARSE;
    }

    uint8_t byte_count = frame[2];
    if ((size_t)(3 + byte_count + 2) > frame_len) {
        return EDGEHMI_ERR_PARSE;
    }
    const uint8_t *data = &frame[3];
    bool is_bit_fc = (blk->func_code == 1 || blk->func_code == 2);

    for (size_t i = 0; i < blk->point_count; ++i) {
        const edgehmi_mb_point_t *pt = &points[blk->point_indices[i]];
        if (pt->reg_addr < blk->start_addr) {
            return EDGEHMI_ERR_PARSE;
        }
        uint16_t reg_off = (uint16_t)(pt->reg_addr - blk->start_addr);
        double raw = 0.0;

        if (is_bit_fc) {
            size_t bi = reg_off / 8u;
            uint8_t bit = (uint8_t)(reg_off % 8u);
            if (bi >= byte_count) {
                return EDGEHMI_ERR_PARSE;
            }
            raw = (double)((data[bi] >> bit) & 0x01u);
        } else {
            size_t boff = (size_t)reg_off * 2u;
            switch (pt->type) {
                case TAG_TYPE_BOOL: {
                    if (boff + 2u > byte_count) return EDGEHMI_ERR_PARSE;
                    uint16_t v = ((uint16_t)data[boff] << 8) | data[boff + 1u];
                    raw = (v != 0) ? 1.0 : 0.0;
                    break;
                }
                case TAG_TYPE_U16: {
                    if (boff + 2u > byte_count) return EDGEHMI_ERR_PARSE;
                    uint16_t v = ((uint16_t)data[boff] << 8) | data[boff + 1u];
                    raw = (double)v;
                    break;
                }
                case TAG_TYPE_I16: {
                    if (boff + 2u > byte_count) return EDGEHMI_ERR_PARSE;
                    uint16_t u = ((uint16_t)data[boff] << 8) | data[boff + 1u];
                    int16_t s = 0;
                    memcpy(&s, &u, sizeof(s));
                    raw = (double)s;
                    break;
                }
                case TAG_TYPE_U32: {
                    if (boff + 4u > byte_count) return EDGEHMI_ERR_PARSE;
                    uint32_t v = decode_u32_order(&data[boff], pt->byte_order);
                    raw = (double)v;
                    break;
                }
                case TAG_TYPE_F32: {
                    if (boff + 4u > byte_count) return EDGEHMI_ERR_PARSE;
                    uint32_t bits = decode_u32_order(&data[boff], pt->byte_order);
                    float f = 0.0f;
                    memcpy(&f, &bits, sizeof(f));
                    raw = (double)f;
                    break;
                }
                default:
                    return EDGEHMI_ERR_INVALID_ARG;
            }
        }

        double scale = (fabs(pt->scale) < 1e-12) ? 1.0 : pt->scale;
        double eng = raw * scale + pt->offset;
        edgehmi_bus_set(bus, pt->tag_idx, eng, TAG_QUAL_GOOD, now_ms);
    }

    return EDGEHMI_OK;
}
