#ifndef EDGEHMI_UI_ENGINE_LVGL_H
#define EDGEHMI_UI_ENGINE_LVGL_H

#include "data_bus.h"
#include "edgehmi_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Build all LVGL 9 screens & widgets from parsed JSON config */
edgehmi_err_t edgehmi_ui_build(const edgehmi_config_t *cfg, edgehmi_bus_t *bus);

/* Incremental dirty-tag refresh: only updates widgets bound to changed tags */
void edgehmi_ui_sync_tick(void);

/* Switch active page by page id */
edgehmi_err_t edgehmi_ui_show_page(const char *page_id);

/* Destroy existing screens prior to config hot-reload */
void edgehmi_ui_destroy(void);

#ifdef __cplusplus
}
#endif

#endif /* EDGEHMI_UI_ENGINE_LVGL_H */
