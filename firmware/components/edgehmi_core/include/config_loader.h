#ifndef EDGEHMI_CONFIG_LOADER_H
#define EDGEHMI_CONFIG_LOADER_H

#include "data_bus.h"
#include "edgehmi_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Parse JSON string into static edgehmi_config_t and register all points on bus */
edgehmi_err_t edgehmi_config_load_json(const char *json_str,
                                       edgehmi_config_t *out_cfg,
                                       edgehmi_bus_t *bus,
                                       char *err_buf,
                                       size_t err_buf_len);

/* Evaluate dynamic threshold color rules for a widget (returns default w->color_rgb if none match) */
uint32_t edgehmi_widget_eval_color(const edgehmi_widget_t *w, double val);

/* Evaluate optional conditional visibility (`visible_when`) for a widget against live bus */
bool edgehmi_widget_eval_visible(const edgehmi_widget_t *w, edgehmi_bus_t *bus);

#ifdef __cplusplus
}
#endif

#endif /* EDGEHMI_CONFIG_LOADER_H */
