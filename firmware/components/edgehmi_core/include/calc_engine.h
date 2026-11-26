#ifndef EDGEHMI_CALC_ENGINE_H
#define EDGEHMI_CALC_ENGINE_H

#include "data_bus.h"
#include "edgehmi_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Resolve source/destination bus tag indices after bus registration */
void edgehmi_calc_bind(edgehmi_calc_tag_t *calcs, size_t count, const edgehmi_bus_t *bus);

/* Deterministic Core 1 tick: compute derived virtual tags (add/sub/mul/div/avg/integrate) */
size_t edgehmi_calc_eval(edgehmi_calc_tag_t *calcs,
                         size_t count,
                         edgehmi_bus_t *bus,
                         uint64_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* EDGEHMI_CALC_ENGINE_H */
