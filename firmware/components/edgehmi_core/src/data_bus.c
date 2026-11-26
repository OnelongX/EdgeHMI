#include "data_bus.h"

#include <math.h>
#include <string.h>

static void bus_lock(edgehmi_bus_t *bus) {
    if (bus && bus->lock_fn) {
        bus->lock_fn(bus->lock_ctx);
    }
}

static void bus_unlock(edgehmi_bus_t *bus) {
    if (bus && bus->unlock_fn) {
        bus->unlock_fn(bus->lock_ctx);
    }
}

static void copy_str(char *dst, size_t cap, const char *src) {
    if (!dst || cap == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t len = strlen(src);
    if (len >= cap) {
        len = cap - 1;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

void edgehmi_bus_init(edgehmi_bus_t *bus,
                      edgehmi_lock_fn lock_fn,
                      edgehmi_lock_fn unlock_fn,
                      void *lock_ctx) {
    if (!bus) {
        return;
    }
    memset(bus, 0, sizeof(*bus));
    bus->lock_fn = lock_fn;
    bus->unlock_fn = unlock_fn;
    bus->lock_ctx = lock_ctx;
}

edgehmi_err_t edgehmi_bus_register(edgehmi_bus_t *bus,
                                   const char *name,
                                   edgehmi_tag_type_t type,
                                   edgehmi_access_t access,
                                   const char *unit,
                                   double initial_val,
                                   int16_t *out_idx) {
    if (!bus || !name || name[0] == '\0') {
        return EDGEHMI_ERR_INVALID_ARG;
    }

    bus_lock(bus);
    for (size_t i = 0; i < bus->count; ++i) {
        if (strcmp(bus->tags[i].name, name) == 0) {
            if (out_idx) {
                *out_idx = (int16_t)i;
            }
            bus_unlock(bus);
            return EDGEHMI_OK;
        }
    }

    if (bus->count >= EDGEHMI_MAX_TAGS) {
        bus_unlock(bus);
        return EDGEHMI_ERR_FULL;
    }

    size_t idx = bus->count++;
    edgehmi_tag_t *t = &bus->tags[idx];
    memset(t, 0, sizeof(*t));
    copy_str(t->name, sizeof(t->name), name);
    copy_str(t->unit, sizeof(t->unit), unit);
    t->type = type;
    t->access = access;
    t->value = initial_val;
    t->quality = TAG_QUAL_INIT;
    t->version = 1;
    t->dirty_ui = true;

    if (out_idx) {
        *out_idx = (int16_t)idx;
    }
    bus_unlock(bus);
    return EDGEHMI_OK;
}

int16_t edgehmi_bus_find(const edgehmi_bus_t *bus, const char *name) {
    if (!bus || !name) {
        return -1;
    }
    for (size_t i = 0; i < bus->count; ++i) {
        if (strcmp(bus->tags[i].name, name) == 0) {
            return (int16_t)i;
        }
    }
    return -1;
}

edgehmi_err_t edgehmi_bus_set(edgehmi_bus_t *bus,
                              int16_t idx,
                              double value,
                              edgehmi_quality_t quality,
                              uint64_t now_ms) {
    if (!bus || idx < 0 || (size_t)idx >= bus->count) {
        return EDGEHMI_ERR_INVALID_ARG;
    }

    bus_lock(bus);
    edgehmi_tag_t *t = &bus->tags[idx];
    bool changed = (fabs(t->value - value) > 1e-9) || (t->quality != quality);
    t->value = value;
    t->quality = quality;
    t->updated_ms = now_ms;
    if (changed) {
        t->version++;
        t->dirty_ui = true;
    }
    bus_unlock(bus);
    return EDGEHMI_OK;
}

edgehmi_err_t edgehmi_bus_get(edgehmi_bus_t *bus,
                              int16_t idx,
                              double *out_val,
                              edgehmi_quality_t *out_qual) {
    return edgehmi_bus_get_snapshot(bus, idx, out_val, out_qual, NULL);
}

edgehmi_err_t edgehmi_bus_get_snapshot(edgehmi_bus_t *bus,
                                       int16_t idx,
                                       double *out_val,
                                       edgehmi_quality_t *out_qual,
                                       uint32_t *out_version) {
    if (!bus || idx < 0 || (size_t)idx >= bus->count) {
        return EDGEHMI_ERR_INVALID_ARG;
    }

    bus_lock(bus);
    const edgehmi_tag_t *t = &bus->tags[idx];
    if (out_val) {
        *out_val = t->value;
    }
    if (out_qual) {
        *out_qual = t->quality;
    }
    if (out_version) {
        *out_version = t->version;
    }
    bus_unlock(bus);
    return EDGEHMI_OK;
}

edgehmi_err_t edgehmi_bus_set_quality(edgehmi_bus_t *bus,
                                      int16_t idx,
                                      edgehmi_quality_t quality,
                                      uint64_t now_ms) {
    if (!bus || idx < 0 || (size_t)idx >= bus->count) {
        return EDGEHMI_ERR_INVALID_ARG;
    }

    bus_lock(bus);
    edgehmi_tag_t *t = &bus->tags[idx];
    bool changed = (t->quality != quality);
    t->quality = quality;
    if (quality == TAG_QUAL_GOOD) {
        t->updated_ms = now_ms;
    }
    if (changed) {
        t->version++;
        t->dirty_ui = true;
    }
    bus_unlock(bus);
    return EDGEHMI_OK;
}

edgehmi_err_t edgehmi_bus_request_write(edgehmi_bus_t *bus,
                                        int16_t idx,
                                        double value,
                                        uint64_t now_ms) {
    if (!bus || idx < 0 || (size_t)idx >= bus->count) {
        return EDGEHMI_ERR_INVALID_ARG;
    }

    bus_lock(bus);
    edgehmi_tag_t *t = &bus->tags[idx];
    if ((t->access & TAG_ACCESS_W) == 0) {
        bus_unlock(bus);
        return EDGEHMI_ERR_READONLY;
    }
    t->pending_write = value;
    t->has_pending_write = true;
    t->value = value;
    t->quality = TAG_QUAL_GOOD;
    t->updated_ms = now_ms;
    t->version++;
    t->dirty_ui = true;
    bus_unlock(bus);
    return EDGEHMI_OK;
}

bool edgehmi_bus_pop_write(edgehmi_bus_t *bus, int16_t idx, double *out_val) {
    if (!bus || idx < 0 || (size_t)idx >= bus->count) {
        return false;
    }

    bus_lock(bus);
    edgehmi_tag_t *t = &bus->tags[idx];
    bool had = t->has_pending_write;
    if (had) {
        if (out_val) {
            *out_val = t->pending_write;
        }
        t->has_pending_write = false;
    }
    bus_unlock(bus);
    return had;
}

bool edgehmi_bus_consume_ui_dirty(edgehmi_bus_t *bus, int16_t idx) {
    if (!bus || idx < 0 || (size_t)idx >= bus->count) {
        return false;
    }

    bus_lock(bus);
    edgehmi_tag_t *t = &bus->tags[idx];
    bool dirty = t->dirty_ui;
    t->dirty_ui = false;
    bus_unlock(bus);
    return dirty;
}

size_t edgehmi_bus_check_stale(edgehmi_bus_t *bus, uint64_t now_ms, uint64_t max_age_ms) {
    if (!bus || max_age_ms == 0) {
        return 0;
    }

    size_t marked = 0;
    bus_lock(bus);
    for (size_t i = 0; i < bus->count; ++i) {
        edgehmi_tag_t *t = &bus->tags[i];
        /* Only polled readable tags can become stale; write-only tags are event-driven */
        if ((t->access & TAG_ACCESS_R) == 0) {
            continue;
        }
        if (t->quality == TAG_QUAL_GOOD && now_ms > t->updated_ms &&
            (now_ms - t->updated_ms) > max_age_ms) {
            t->quality = TAG_QUAL_STALE;
            t->version++;
            t->dirty_ui = true;
            marked++;
        }
    }
    bus_unlock(bus);
    return marked;
}

bool edgehmi_ct_str_eq(const char *a, const char *b) {
    if (!a || !b) {
        return false;
    }
    size_t la = strlen(a);
    size_t lb = strlen(b);
    size_t max_len = la > lb ? la : lb;
    volatile uint8_t diff = (uint8_t)(la != lb);
    for (size_t i = 0; i < max_len; ++i) {
        uint8_t ca = (i < la) ? (uint8_t)a[i] : 0u;
        uint8_t cb = (i < lb) ? (uint8_t)b[i] : 0u;
        diff |= (uint8_t)(ca ^ cb);
    }
    return diff == 0u;
}

size_t edgehmi_json_escape(const char *src, char *dst, size_t dst_cap) {
    if (!dst || dst_cap == 0) {
        return 0;
    }
    if (!src) {
        dst[0] = '\0';
        return 0;
    }
    size_t w = 0;
    for (size_t i = 0; src[i] != '\0'; ++i) {
        unsigned char c = (unsigned char)src[i];
        const char *esc = NULL;
        switch (c) {
            case '"':  esc = "\\\""; break;
            case '\\': esc = "\\\\"; break;
            case '\n': esc = "\\n";  break;
            case '\r': esc = "\\r";  break;
            case '\t': esc = "\\t";  break;
            default:   break;
        }
        if (esc) {
            if (w + 2 >= dst_cap) {
                break;
            }
            dst[w++] = esc[0];
            dst[w++] = esc[1];
        } else if (c < 0x20u) {
            /* Strip other ASCII control chars */
            continue;
        } else {
            if (w + 1 >= dst_cap) {
                break;
            }
            dst[w++] = (char)c;
        }
    }
    dst[w] = '\0';
    return w;
}

void edgehmi_nvs_safe_key(const char *tag_name, char out_key[16]) {
    if (!out_key) {
        return;
    }
    if (!tag_name || tag_name[0] == '\0') {
        out_key[0] = '\0';
        return;
    }
    size_t len = strlen(tag_name);
    if (len <= 15) {
        memcpy(out_key, tag_name, len + 1);
        return;
    }
    /* FNV-1a 32-bit hash -> 8-char prefix + '_' + 6 hex digits = 15 chars */
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; ++i) {
        h ^= (uint8_t)tag_name[i];
        h *= 16777619u;
    }
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < 8; ++i) {
        out_key[i] = tag_name[i];
    }
    out_key[8] = '_';
    uint32_t h24 = (h ^ (h >> 24)) & 0xFFFFFFu;
    out_key[9]  = hex[(h24 >> 20) & 0xFu];
    out_key[10] = hex[(h24 >> 16) & 0xFu];
    out_key[11] = hex[(h24 >> 12) & 0xFu];
    out_key[12] = hex[(h24 >> 8) & 0xFu];
    out_key[13] = hex[(h24 >> 4) & 0xFu];
    out_key[14] = hex[h24 & 0xFu];
    out_key[15] = '\0';
}
