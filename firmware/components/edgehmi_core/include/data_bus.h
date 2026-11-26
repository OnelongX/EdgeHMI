#ifndef EDGEHMI_DATA_BUS_H
#define EDGEHMI_DATA_BUS_H

#include "edgehmi_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*edgehmi_lock_fn)(void *ctx);

typedef struct {
    edgehmi_tag_t tags[EDGEHMI_MAX_TAGS];
    size_t count;
    edgehmi_lock_fn lock_fn;
    edgehmi_lock_fn unlock_fn;
    void *lock_ctx;
} edgehmi_bus_t;

void edgehmi_bus_init(edgehmi_bus_t *bus,
                      edgehmi_lock_fn lock_fn,
                      edgehmi_lock_fn unlock_fn,
                      void *lock_ctx);

edgehmi_err_t edgehmi_bus_register(edgehmi_bus_t *bus,
                                   const char *name,
                                   edgehmi_tag_type_t type,
                                   edgehmi_access_t access,
                                   const char *unit,
                                   double initial_val,
                                   int16_t *out_idx);

int16_t edgehmi_bus_find(const edgehmi_bus_t *bus, const char *name);

edgehmi_err_t edgehmi_bus_set(edgehmi_bus_t *bus,
                              int16_t idx,
                              double value,
                              edgehmi_quality_t quality,
                              uint64_t now_ms);

edgehmi_err_t edgehmi_bus_get(edgehmi_bus_t *bus,
                              int16_t idx,
                              double *out_val,
                              edgehmi_quality_t *out_qual);

/* Non-destructive atomic read including monotonic version counter for multi-widget UI sync */
edgehmi_err_t edgehmi_bus_get_snapshot(edgehmi_bus_t *bus,
                                       int16_t idx,
                                       double *out_val,
                                       edgehmi_quality_t *out_qual,
                                       uint32_t *out_version);

/* Update quality stamp without overwriting last numeric value (e.g. Modbus comm failure) */
edgehmi_err_t edgehmi_bus_set_quality(edgehmi_bus_t *bus,
                                      int16_t idx,
                                      edgehmi_quality_t quality,
                                      uint64_t now_ms);

/* Queue a write command from UI/Rule/Cloud toward the fieldbus (Modbus) */
edgehmi_err_t edgehmi_bus_request_write(edgehmi_bus_t *bus,
                                        int16_t idx,
                                        double value,
                                        uint64_t now_ms);

/* Consume a pending write request for fieldbus execution */
bool edgehmi_bus_pop_write(edgehmi_bus_t *bus, int16_t idx, double *out_val);

/* Legacy UI dirty flag helper (prefer edgehmi_bus_get_snapshot + per-widget version) */
bool edgehmi_bus_consume_ui_dirty(edgehmi_bus_t *bus, int16_t idx);

/* Mark stale readable tags whose last update exceeded max_age_ms */
size_t edgehmi_bus_check_stale(edgehmi_bus_t *bus, uint64_t now_ms, uint64_t max_age_ms);

/* Security & storage utilities shared across core and firmware */
bool edgehmi_ct_str_eq(const char *a, const char *b);
size_t edgehmi_json_escape(const char *src, char *dst, size_t dst_cap);
void edgehmi_nvs_safe_key(const char *tag_name, char out_key[16]);

#ifdef __cplusplus
}
#endif

#endif /* EDGEHMI_DATA_BUS_H */
