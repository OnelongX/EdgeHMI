#include "can_codec.h"

edgehmi_err_t edgehmi_can_decode_signal(const edgehmi_can_signal_t *sig,
                                        const uint8_t *data,
                                        uint8_t dlc,
                                        double *out_eng_val) {
    if (!sig || !data || !out_eng_val || dlc > 8 || sig->bit_len == 0 || sig->bit_len > 32) {
        return EDGEHMI_ERR_INVALID_ARG;
    }

    uint64_t raw = 0;
    if (!sig->big_endian) {
        /* Intel (little-endian) bit numbering: start_bit is LSB */
        if ((uint32_t)sig->start_bit + (uint32_t)sig->bit_len > (uint32_t)dlc * 8u) {
            return EDGEHMI_ERR_PARSE;
        }
        for (uint8_t i = 0; i < sig->bit_len; ++i) {
            uint8_t bit_pos = (uint8_t)(sig->start_bit + i);
            uint8_t byte_idx = (uint8_t)(bit_pos / 8u);
            uint8_t bit_in_byte = (uint8_t)(bit_pos % 8u);
            if ((data[byte_idx] >> bit_in_byte) & 1u) {
                raw |= (1ULL << i);
            }
        }
    } else {
        /* Motorola (big-endian / DBC MSB-first) bit numbering: start_bit is MSB */
        int byte_idx = (int)(sig->start_bit / 8u);
        int bit_in_byte = (int)(sig->start_bit % 8u);
        for (uint8_t i = 0; i < sig->bit_len; ++i) {
            if (byte_idx < 0 || byte_idx >= (int)dlc) {
                return EDGEHMI_ERR_PARSE;
            }
            raw = (raw << 1) | (uint64_t)((data[byte_idx] >> bit_in_byte) & 1u);
            if (bit_in_byte == 0) {
                bit_in_byte = 7;
                byte_idx++;
            } else {
                bit_in_byte--;
            }
        }
    }

    double num = 0.0;
    if (sig->is_signed && sig->bit_len < 64) {
        uint64_t sign_bit = 1ULL << (sig->bit_len - 1u);
        if (raw & sign_bit) {
            uint64_t mask = (sig->bit_len == 32) ? 0xFFFFFFFFULL : ((1ULL << sig->bit_len) - 1ULL);
            int64_t sval = (int64_t)(raw | ~mask);
            num = (double)sval;
        } else {
            num = (double)raw;
        }
    } else {
        num = (double)raw;
    }

    double scale = (sig->scale != 0.0) ? sig->scale : 1.0;
    *out_eng_val = num * scale + sig->offset;
    return EDGEHMI_OK;
}

size_t edgehmi_can_ingest_frame(const edgehmi_can_cfg_t *can_cfg,
                                uint32_t can_id,
                                bool ext_id,
                                const uint8_t *data,
                                uint8_t dlc,
                                edgehmi_bus_t *bus,
                                uint64_t now_ms) {
    if (!can_cfg || !can_cfg->enabled || !data || !bus) {
        return 0;
    }
    size_t updated = 0;
    for (size_t i = 0; i < can_cfg->signal_count; ++i) {
        const edgehmi_can_signal_t *sig = &can_cfg->signals[i];
        if (sig->can_id == can_id && sig->ext_id == ext_id && sig->tag_idx >= 0) {
            double val = 0.0;
            if (edgehmi_can_decode_signal(sig, data, dlc, &val) == EDGEHMI_OK) {
                if (edgehmi_bus_set(bus, sig->tag_idx, val, TAG_QUAL_GOOD, now_ms) == EDGEHMI_OK) {
                    updated++;
                }
            }
        }
    }
    return updated;
}
