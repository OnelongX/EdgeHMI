#include "ui_engine_lvgl.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "edgehmi_ui";

/* Optional CJK subset font hook: define EDGEHMI_CUSTOM_FONT in build flags (e.g. font_noto_sc_16) */
#ifdef EDGEHMI_CUSTOM_FONT
LV_FONT_DECLARE(EDGEHMI_CUSTOM_FONT);
#define EDGEHMI_UI_FONT (&EDGEHMI_CUSTOM_FONT)
#else
#define EDGEHMI_UI_FONT LV_FONT_DEFAULT
#endif

typedef struct {
    const edgehmi_widget_t *meta;
    lv_obj_t *obj;
    lv_obj_t *aux_label;
    lv_chart_series_t *chart_ser;
    uint32_t last_version;
} ui_binding_t;

typedef struct {
    char id[EDGEHMI_MAX_NAME_LEN];
    lv_obj_t *screen;
    ui_binding_t bindings[EDGEHMI_MAX_WIDGETS_PER_PG];
    size_t binding_count;
} ui_page_runtime_t;

static const edgehmi_config_t *s_cfg = NULL;
static edgehmi_bus_t *s_bus = NULL;
static edgehmi_history_t *s_hist = NULL;
static uint32_t s_last_alarm_seq = 0;
static ui_page_runtime_t s_pages[EDGEHMI_MAX_PAGES];
static size_t s_page_count = 0;
static size_t s_active_page = 0;
static lv_obj_t *s_comm_banner = NULL;

typedef struct {
    const edgehmi_widget_t *widget;
    lv_obj_t *overlay;
    lv_obj_t *pin_ta;
    lv_obj_t *err_lbl;
} pin_modal_ctx_t;

static pin_modal_ctx_t s_modal = {0};

static const char *s_pin_kb_map[] = {
    "1", "2", "3", "\n",
    "4", "5", "6", "\n",
    "7", "8", "9", "\n",
    "CLR", "0", "OK", ""
};

static uint64_t now_ms(void) {
    return (uint64_t)(esp_timer_get_time() / 1000LL);
}

static void close_pin_modal(void) {
    if (s_modal.overlay) {
        lv_obj_delete(s_modal.overlay);
    }
    memset(&s_modal, 0, sizeof(s_modal));
}

static void on_modal_cancel(lv_event_t *e) {
    (void)e;
    close_pin_modal();
}

static void execute_modal_write(void) {
    const edgehmi_widget_t *w = s_modal.widget;
    if (!w || !s_bus) {
        close_pin_modal();
        return;
    }
    if (w->pin_required && s_cfg && s_modal.pin_ta) {
        const char *entered = lv_textarea_get_text(s_modal.pin_ta);
        if (!entered || !edgehmi_ct_str_eq(entered, s_cfg->engineer_pin)) {
            if (s_modal.err_lbl) {
                lv_label_set_text(s_modal.err_lbl, "Invalid Engineer PIN!");
            }
            lv_textarea_set_text(s_modal.pin_ta, "");
            return;
        }
    }
    if (w->bind_tag_idx >= 0 && w->has_write_value) {
        edgehmi_bus_request_write(s_bus, w->bind_tag_idx, w->write_value, now_ms());
    }
    close_pin_modal();
}

static void on_modal_confirm(lv_event_t *e) {
    (void)e;
    execute_modal_write();
}

static void on_pin_kb_event(lv_event_t *e) {
    lv_obj_t *btnm = lv_event_get_target_obj(e);
    uint32_t id = lv_buttonmatrix_get_selected_button(btnm);
    const char *txt = lv_buttonmatrix_get_button_text(btnm, id);
    if (!txt || !s_modal.pin_ta) return;

    if (strcmp(txt, "CLR") == 0) {
        lv_textarea_set_text(s_modal.pin_ta, "");
    } else if (strcmp(txt, "OK") == 0) {
        execute_modal_write();
    } else {
        lv_textarea_add_text(s_modal.pin_ta, txt);
    }
}

static void show_confirm_modal(const edgehmi_widget_t *w) {
    close_pin_modal();
    lv_obj_t *top = lv_layer_top();
    lv_obj_t *ov = lv_obj_create(top);
    lv_obj_set_size(ov, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(ov, lv_color_hex(0x000000u), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ov, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_border_width(ov, 0, LV_PART_MAIN);
    lv_obj_remove_flag(ov, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *box = lv_obj_create(ov);
    lv_obj_set_size(box, 360, w->pin_required ? 340 : 180);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x0F172Au), LV_PART_MAIN);
    lv_obj_set_style_border_color(box, lv_color_hex(0xEF4444u), LV_PART_MAIN);
    lv_obj_set_style_border_width(box, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(box, 12, LV_PART_MAIN);
    lv_obj_center(box);

    lv_obj_t *title = lv_label_create(box);
    lv_label_set_text(title, w->pin_required ? "Engineer PIN Required" : "Confirm Critical Action");
    lv_obj_set_style_text_color(title, lv_color_hex(0xF87171u), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 4);

    lv_obj_t *msg = lv_label_create(box);
    lv_label_set_text(msg, w->confirm_text[0] ? w->confirm_text : "Execute critical operation?");
    lv_obj_set_style_text_color(msg, lv_color_hex(0xE2E8F0u), LV_PART_MAIN);
    lv_obj_align(msg, LV_ALIGN_TOP_MID, 0, 34);

    s_modal.widget = w;
    s_modal.overlay = ov;

    if (w->pin_required) {
        s_modal.pin_ta = lv_textarea_create(box);
        lv_textarea_set_one_line(s_modal.pin_ta, true);
        lv_textarea_set_password_mode(s_modal.pin_ta, true);
        lv_textarea_set_max_length(s_modal.pin_ta, 8);
        lv_obj_set_size(s_modal.pin_ta, 240, 40);
        lv_obj_align(s_modal.pin_ta, LV_ALIGN_TOP_MID, 0, 66);

        s_modal.err_lbl = lv_label_create(box);
        lv_label_set_text(s_modal.err_lbl, "");
        lv_obj_set_style_text_color(s_modal.err_lbl, lv_color_hex(0xFCA5A5u), LV_PART_MAIN);
        lv_obj_align(s_modal.err_lbl, LV_ALIGN_TOP_MID, 0, 110);

        lv_obj_t *kb = lv_buttonmatrix_create(box);
        lv_buttonmatrix_set_map(kb, s_pin_kb_map);
        lv_obj_set_size(kb, 260, 145);
        lv_obj_align(kb, LV_ALIGN_TOP_MID, 0, 134);
        lv_obj_add_event_cb(kb, on_pin_kb_event, LV_EVENT_VALUE_CHANGED, NULL);

        lv_obj_t *btn_cancel = lv_button_create(box);
        lv_obj_set_size(btn_cancel, 120, 34);
        lv_obj_align(btn_cancel, LV_ALIGN_BOTTOM_MID, 0, -2);
        lv_obj_add_event_cb(btn_cancel, on_modal_cancel, LV_EVENT_CLICKED, NULL);
        lv_obj_t *cl = lv_label_create(btn_cancel);
        lv_label_set_text(cl, "CANCEL");
        lv_obj_center(cl);
    } else {
        lv_obj_t *btn_ok = lv_button_create(box);
        lv_obj_set_size(btn_ok, 120, 40);
        lv_obj_set_style_bg_color(btn_ok, lv_color_hex(0xEF4444u), LV_PART_MAIN);
        lv_obj_align(btn_ok, LV_ALIGN_BOTTOM_LEFT, 24, -8);
        lv_obj_add_event_cb(btn_ok, on_modal_confirm, LV_EVENT_CLICKED, NULL);
        lv_obj_t *ol = lv_label_create(btn_ok);
        lv_label_set_text(ol, "CONFIRM");
        lv_obj_center(ol);

        lv_obj_t *btn_cancel = lv_button_create(box);
        lv_obj_set_size(btn_cancel, 120, 40);
        lv_obj_align(btn_cancel, LV_ALIGN_BOTTOM_RIGHT, -24, -8);
        lv_obj_add_event_cb(btn_cancel, on_modal_cancel, LV_EVENT_CLICKED, NULL);
        lv_obj_t *cl = lv_label_create(btn_cancel);
        lv_label_set_text(cl, "CANCEL");
        lv_obj_center(cl);
    }
}

static void on_button_click(lv_event_t *e) {
    const edgehmi_widget_t *w = (const edgehmi_widget_t *)lv_event_get_user_data(e);
    if (!w || !s_bus) return;

    if (w->nav_page[0] != '\0') {
        edgehmi_ui_show_page(w->nav_page);
    }
    if (w->bind_tag_idx >= 0 && w->has_write_value) {
        if (w->confirm || w->pin_required) {
            show_confirm_modal(w);
        } else {
            edgehmi_bus_request_write(s_bus, w->bind_tag_idx, w->write_value, now_ms());
        }
    }
}

static void on_switch_change(lv_event_t *e) {
    const edgehmi_widget_t *w = (const edgehmi_widget_t *)lv_event_get_user_data(e);
    lv_obj_t *sw = lv_event_get_target_obj(e);
    if (!w || !s_bus || w->bind_tag_idx < 0) return;

    bool checked = lv_obj_has_state(sw, LV_STATE_CHECKED);
    edgehmi_bus_request_write(s_bus, w->bind_tag_idx, checked ? 1.0 : 0.0, now_ms());
}

static void on_slider_released(lv_event_t *e) {
    const edgehmi_widget_t *w = (const edgehmi_widget_t *)lv_event_get_user_data(e);
    lv_obj_t *sl = lv_event_get_target_obj(e);
    if (!w || !s_bus || w->bind_tag_idx < 0) return;

    int32_t v = lv_slider_get_value(sl);
    edgehmi_bus_request_write(s_bus, w->bind_tag_idx, (double)v, now_ms());
}

static void on_spinbox_delta(const edgehmi_widget_t *w, double sign) {
    if (!w || !s_bus || w->bind_tag_idx < 0) return;
    double cur = 0.0;
    edgehmi_bus_get(s_bus, w->bind_tag_idx, &cur, NULL);
    double step = (w->step > 0.0) ? w->step : 1.0;
    double nxt = cur + sign * step;
    if (nxt < w->min_val) nxt = w->min_val;
    if (nxt > w->max_val) nxt = w->max_val;
    edgehmi_bus_request_write(s_bus, w->bind_tag_idx, nxt, now_ms());
}

static void on_spinbox_dec(lv_event_t *e) {
    on_spinbox_delta((const edgehmi_widget_t *)lv_event_get_user_data(e), -1.0);
}

static void on_spinbox_inc(lv_event_t *e) {
    on_spinbox_delta((const edgehmi_widget_t *)lv_event_get_user_data(e), 1.0);
}

static void on_alarm_ack_all(lv_event_t *e) {
    (void)e;
    if (s_hist) {
        edgehmi_history_ack_alarm(s_hist, 0);
        s_last_alarm_seq = 0; /* Force refresh */
    }
}

static void format_value(const edgehmi_widget_t *w, double val, char *buf, size_t cap) {
    if (w->unit[0] != '\0') {
        snprintf(buf, cap, "%.*f %s", (int)w->decimals, val, w->unit);
    } else {
        snprintf(buf, cap, "%.*f", (int)w->decimals, val);
    }
}

static void apply_binding_value(ui_binding_t *b, double val, edgehmi_quality_t qual) {
    const edgehmi_widget_t *w = b->meta;
    char txt[64];

    switch (w->type) {
        case WIDGET_VALUE:
            if (qual == TAG_QUAL_BAD || qual == TAG_QUAL_STALE) {
                snprintf(txt, sizeof(txt), "-- %s", w->unit);
            } else {
                format_value(w, val, txt, sizeof(txt));
            }
            lv_label_set_text(b->obj, txt);
            break;

        case WIDGET_GAUGE: {
            int32_t iv = (int32_t)lround(val);
            lv_arc_set_value(b->obj, iv);
            if (b->aux_label) {
                format_value(w, val, txt, sizeof(txt));
                lv_label_set_text(b->aux_label, txt);
            }
            break;
        }

        case WIDGET_BAR: {
            int32_t iv = (int32_t)lround(val);
            lv_bar_set_value(b->obj, iv, LV_ANIM_OFF);
            break;
        }

        case WIDGET_SWITCH:
            if (fabs(val) > 0.5) {
                lv_obj_add_state(b->obj, LV_STATE_CHECKED);
            } else {
                lv_obj_remove_state(b->obj, LV_STATE_CHECKED);
            }
            break;

        case WIDGET_BADGE: {
            bool on = fabs(val) > 0.5;
            const char *label = on
                ? (w->true_text[0] ? w->true_text : "ACTIVE")
                : (w->false_text[0] ? w->false_text : "IDLE");
            if (b->aux_label) {
                lv_label_set_text(b->aux_label, label);
            }
            lv_obj_set_style_bg_color(
                b->obj,
                lv_color_hex(on ? w->color_rgb : 0x334155u),
                LV_PART_MAIN);
            break;
        }

        case WIDGET_CHART:
            if (b->chart_ser) {
                lv_chart_set_next_value(b->obj, b->chart_ser, (int32_t)lround(val));
            }
            break;

        case WIDGET_SLIDER:
            lv_slider_set_value(b->obj, (int32_t)lround(val), LV_ANIM_OFF);
            if (b->aux_label) {
                format_value(w, val, txt, sizeof(txt));
                lv_label_set_text(b->aux_label, txt);
            }
            break;

        case WIDGET_SPINBOX:
            if (b->aux_label) {
                format_value(w, val, txt, sizeof(txt));
                lv_label_set_text(b->aux_label, txt);
            }
            break;

        default:
            break;
    }
}

void edgehmi_ui_destroy(void) {
    close_pin_modal();
    if (s_comm_banner) {
        lv_obj_delete(s_comm_banner);
        s_comm_banner = NULL;
    }
    for (size_t i = 0; i < s_page_count; ++i) {
        if (s_pages[i].screen) {
            lv_obj_delete(s_pages[i].screen);
            s_pages[i].screen = NULL;
        }
    }
    memset(s_pages, 0, sizeof(s_pages));
    s_page_count = 0;
    s_active_page = 0;
}

edgehmi_err_t edgehmi_ui_build(const edgehmi_config_t *cfg,
                               edgehmi_bus_t *bus,
                               edgehmi_history_t *hist) {
    if (!cfg || !bus) {
        return EDGEHMI_ERR_INVALID_ARG;
    }

    /* Preserve old LVGL screen handles so we load the new screen BEFORE deleting the active one */
    close_pin_modal();
    lv_obj_t *old_screens[EDGEHMI_MAX_PAGES] = {0};
    size_t old_count = s_page_count;
    for (size_t i = 0; i < old_count && i < EDGEHMI_MAX_PAGES; ++i) {
        old_screens[i] = s_pages[i].screen;
    }
    lv_obj_t *old_banner = s_comm_banner;
    s_comm_banner = NULL;

    memset(s_pages, 0, sizeof(s_pages));
    s_page_count = 0;
    s_active_page = 0;
    s_cfg = cfg;
    s_bus = bus;
    s_hist = hist;
    s_last_alarm_seq = 0;

    uint32_t bg_hex = cfg->dark_theme ? 0x0B1220u : 0xF8FAFCu;
    uint32_t card_hex = cfg->dark_theme ? 0x111C2Eu : 0xFFFFFFu;
    uint32_t text_hex = cfg->dark_theme ? 0xE2E8F0u : 0x0F172Au;

    for (size_t p = 0; p < cfg->page_count; ++p) {
        const edgehmi_page_t *pg = &cfg->pages[p];
        ui_page_runtime_t *rt = &s_pages[p];
        strncpy(rt->id, pg->id, sizeof(rt->id) - 1);

        lv_obj_t *scr = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(scr, lv_color_hex(bg_hex), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_text_font(scr, EDGEHMI_UI_FONT, LV_PART_MAIN);
        rt->screen = scr;

        for (size_t i = 0; i < pg->widget_count; ++i) {
            const edgehmi_widget_t *w = &pg->widgets[i];
            ui_binding_t *b = &rt->bindings[rt->binding_count++];
            memset(b, 0, sizeof(*b));
            b->meta = w;

            lv_obj_t *obj = NULL;
            switch (w->type) {
                case WIDGET_CARD:
                    obj = lv_obj_create(scr);
                    lv_obj_set_style_bg_color(obj, lv_color_hex(card_hex), LV_PART_MAIN);
                    lv_obj_set_style_border_color(obj, lv_color_hex(0x1E293Bu), LV_PART_MAIN);
                    lv_obj_set_style_border_width(obj, 1, LV_PART_MAIN);
                    lv_obj_set_style_radius(obj, 10, LV_PART_MAIN);
                    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
                    if (w->text[0] != '\0') {
                        lv_obj_t *hdr = lv_label_create(obj);
                        lv_label_set_text(hdr, w->text);
                        lv_obj_set_style_text_color(hdr, lv_color_hex(0x94A3B8u), LV_PART_MAIN);
                        lv_obj_align(hdr, LV_ALIGN_TOP_LEFT, 4, 2);
                    }
                    break;

                case WIDGET_LABEL:
                    obj = lv_label_create(scr);
                    lv_label_set_text(obj, w->text);
                    lv_obj_set_style_text_color(obj, lv_color_hex(text_hex), LV_PART_MAIN);
                    break;

                case WIDGET_VALUE:
                    obj = lv_label_create(scr);
                    lv_label_set_text(obj, "--");
                    lv_obj_set_style_text_color(obj, lv_color_hex(w->color_rgb), LV_PART_MAIN);
                    break;

                case WIDGET_GAUGE:
                    obj = lv_arc_create(scr);
                    lv_arc_set_range(obj, (int32_t)lround(w->min_val), (int32_t)lround(w->max_val));
                    lv_arc_set_bg_angles(obj, 135, 45);
                    lv_obj_remove_style(obj, NULL, LV_PART_KNOB);
                    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
                    lv_obj_set_style_arc_color(obj, lv_color_hex(w->color_rgb), LV_PART_INDICATOR);
                    b->aux_label = lv_label_create(obj);
                    lv_obj_set_style_text_color(b->aux_label, lv_color_hex(text_hex), LV_PART_MAIN);
                    lv_obj_center(b->aux_label);
                    break;

                case WIDGET_BAR:
                    obj = lv_bar_create(scr);
                    lv_bar_set_range(obj, (int32_t)lround(w->min_val), (int32_t)lround(w->max_val));
                    lv_obj_set_style_bg_color(obj, lv_color_hex(w->color_rgb), LV_PART_INDICATOR);
                    break;

                case WIDGET_BUTTON:
                    obj = lv_button_create(scr);
                    lv_obj_set_style_bg_color(obj, lv_color_hex(w->color_rgb), LV_PART_MAIN);
                    lv_obj_set_style_radius(obj, 8, LV_PART_MAIN);
                    lv_obj_add_event_cb(obj, on_button_click, LV_EVENT_CLICKED, (void *)w);
                    b->aux_label = lv_label_create(obj);
                    lv_label_set_text(b->aux_label, w->text);
                    lv_obj_center(b->aux_label);
                    break;

                case WIDGET_SWITCH:
                    obj = lv_switch_create(scr);
                    lv_obj_add_event_cb(obj, on_switch_change, LV_EVENT_VALUE_CHANGED, (void *)w);
                    break;

                case WIDGET_BADGE:
                    obj = lv_obj_create(scr);
                    lv_obj_set_style_radius(obj, 14, LV_PART_MAIN);
                    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
                    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
                    b->aux_label = lv_label_create(obj);
                    lv_obj_set_style_text_color(b->aux_label, lv_color_hex(0xFFFFFFu), LV_PART_MAIN);
                    lv_obj_center(b->aux_label);
                    break;

                case WIDGET_CHART:
                    obj = lv_chart_create(scr);
                    lv_chart_set_type(obj, LV_CHART_TYPE_LINE);
                    lv_chart_set_point_count(obj, 24);
                    lv_chart_set_range(obj, LV_CHART_AXIS_PRIMARY_Y,
                                       (int32_t)lround(w->min_val),
                                       (int32_t)lround(w->max_val));
                    b->chart_ser = lv_chart_add_series(obj, lv_color_hex(w->color_rgb),
                                                       LV_CHART_AXIS_PRIMARY_Y);
                    break;

                case WIDGET_SLIDER:
                    obj = lv_slider_create(scr);
                    lv_slider_set_range(obj, (int32_t)lround(w->min_val), (int32_t)lround(w->max_val));
                    lv_obj_set_style_bg_color(obj, lv_color_hex(w->color_rgb), LV_PART_INDICATOR);
                    lv_obj_add_event_cb(obj, on_slider_released, LV_EVENT_RELEASED, (void *)w);
                    break;

                case WIDGET_SPINBOX: {
                    obj = lv_obj_create(scr);
                    lv_obj_set_style_bg_color(obj, lv_color_hex(card_hex), LV_PART_MAIN);
                    lv_obj_set_style_border_color(obj, lv_color_hex(0x334155u), LV_PART_MAIN);
                    lv_obj_set_style_border_width(obj, 1, LV_PART_MAIN);
                    lv_obj_set_style_radius(obj, 8, LV_PART_MAIN);
                    lv_obj_set_style_pad_all(obj, 4, LV_PART_MAIN);
                    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);

                    lv_obj_t *bdec = lv_button_create(obj);
                    lv_obj_set_size(bdec, 36, LV_PCT(100));
                    lv_obj_align(bdec, LV_ALIGN_LEFT_MID, 0, 0);
                    lv_obj_add_event_cb(bdec, on_spinbox_dec, LV_EVENT_CLICKED, (void *)w);
                    lv_obj_t *dl = lv_label_create(bdec);
                    lv_label_set_text(dl, "-");
                    lv_obj_center(dl);

                    b->aux_label = lv_label_create(obj);
                    lv_label_set_text(b->aux_label, "--");
                    lv_obj_set_style_text_color(b->aux_label, lv_color_hex(w->color_rgb), LV_PART_MAIN);
                    lv_obj_center(b->aux_label);

                    lv_obj_t *binc = lv_button_create(obj);
                    lv_obj_set_size(binc, 36, LV_PCT(100));
                    lv_obj_align(binc, LV_ALIGN_RIGHT_MID, 0, 0);
                    lv_obj_add_event_cb(binc, on_spinbox_inc, LV_EVENT_CLICKED, (void *)w);
                    lv_obj_t *il = lv_label_create(binc);
                    lv_label_set_text(il, "+");
                    lv_obj_center(il);
                    break;
                }

                case WIDGET_ALARM_LIST: {
                    obj = lv_obj_create(scr);
                    lv_obj_set_style_bg_color(obj, lv_color_hex(card_hex), LV_PART_MAIN);
                    lv_obj_set_style_border_color(obj, lv_color_hex(0x334155u), LV_PART_MAIN);
                    lv_obj_set_style_border_width(obj, 1, LV_PART_MAIN);
                    lv_obj_set_style_radius(obj, 8, LV_PART_MAIN);
                    b->aux_label = lv_label_create(obj);
                    lv_label_set_text(b->aux_label, "No active alarms");
                    lv_obj_set_style_text_color(b->aux_label, lv_color_hex(text_hex), LV_PART_MAIN);
                    lv_obj_align(b->aux_label, LV_ALIGN_TOP_LEFT, 4, 4);

                    lv_obj_t *ack = lv_button_create(obj);
                    lv_obj_set_size(ack, 76, 26);
                    lv_obj_align(ack, LV_ALIGN_TOP_RIGHT, -2, 0);
                    lv_obj_add_event_cb(ack, on_alarm_ack_all, LV_EVENT_CLICKED, NULL);
                    lv_obj_t *al = lv_label_create(ack);
                    lv_label_set_text(al, "ACK");
                    lv_obj_center(al);
                    break;
                }

                case WIDGET_QRCODE: {
                    const char *qr_txt = w->src[0] ? w->src : (w->text[0] ? w->text : "http://edgehmi.local");
#if defined(LV_USE_QRCODE) && LV_USE_QRCODE
                    int32_t qsz = w->w < w->h ? w->w : w->h;
                    obj = lv_qrcode_create(scr);
                    lv_qrcode_set_size(obj, qsz);
                    lv_qrcode_set_dark_color(obj, lv_color_hex(0x0F172Au));
                    lv_qrcode_set_light_color(obj, lv_color_hex(0xFFFFFFu));
                    lv_qrcode_update(obj, qr_txt, (uint32_t)strlen(qr_txt));
#else
                    obj = lv_obj_create(scr);
                    lv_obj_set_style_bg_color(obj, lv_color_hex(0xFFFFFFu), LV_PART_MAIN);
                    lv_obj_t *ql = lv_label_create(obj);
                    lv_label_set_text(ql, qr_txt);
                    lv_obj_set_style_text_color(ql, lv_color_hex(0x0F172Au), LV_PART_MAIN);
                    lv_obj_center(ql);
#endif
                    break;
                }

                case WIDGET_IMAGE: {
                    obj = lv_obj_create(scr);
                    lv_obj_set_style_bg_color(obj, lv_color_hex(card_hex), LV_PART_MAIN);
                    lv_obj_set_style_border_color(obj, lv_color_hex(w->color_rgb), LV_PART_MAIN);
                    lv_obj_set_style_border_width(obj, 1, LV_PART_MAIN);
                    lv_obj_set_style_radius(obj, 8, LV_PART_MAIN);
                    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
                    lv_obj_t *im = lv_label_create(obj);
                    lv_label_set_text(im, w->text[0] ? w->text : (w->src[0] ? w->src : "IMG"));
                    lv_obj_set_style_text_color(im, lv_color_hex(w->color_rgb), LV_PART_MAIN);
                    lv_obj_center(im);
                    break;
                }
            }

            if (obj) {
                lv_obj_set_pos(obj, w->x, w->y);
                lv_obj_set_size(obj, w->w, w->h);
                b->obj = obj;
            }
        }
        s_page_count++;
    }

    if (s_page_count > 0) {
        s_active_page = 0;
        lv_screen_load(s_pages[0].screen);
        /* Top-layer RS485 Communication Loss Fail-Safe Banner (hidden when healthy) */
        s_comm_banner = lv_obj_create(lv_layer_top());
        lv_obj_set_size(s_comm_banner, cfg->width, 32);
        lv_obj_align(s_comm_banner, LV_ALIGN_TOP_MID, 0, 0);
        lv_obj_set_style_bg_color(s_comm_banner, lv_color_hex(0xDC2626u), LV_PART_MAIN);
        lv_obj_set_style_border_width(s_comm_banner, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(s_comm_banner, 0, LV_PART_MAIN);
        lv_obj_remove_flag(s_comm_banner, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *bl = lv_label_create(s_comm_banner);
        lv_label_set_text(bl, "RS485 COMM LOSS — FAIL-SAFE ACTIVE");
        lv_obj_set_style_text_color(bl, lv_color_hex(0xFFFFFFu), LV_PART_MAIN);
        lv_obj_center(bl);
        lv_obj_add_flag(s_comm_banner, LV_OBJ_FLAG_HIDDEN);

        ESP_LOGI(TAG, "Built %u HMI pages from JSON config v%s",
                 (unsigned)s_page_count, cfg->version);
    }

    /* Safely delete previous screens & banner now that the new screen is active */
    if (old_banner) {
        lv_obj_delete(old_banner);
    }
    for (size_t i = 0; i < old_count && i < EDGEHMI_MAX_PAGES; ++i) {
        if (old_screens[i]) {
            lv_obj_delete(old_screens[i]);
        }
    }
    return EDGEHMI_OK;
}

edgehmi_err_t edgehmi_ui_show_page(const char *page_id) {
    if (!page_id) return EDGEHMI_ERR_INVALID_ARG;
    for (size_t i = 0; i < s_page_count; ++i) {
        if (strcmp(s_pages[i].id, page_id) == 0) {
            s_active_page = i;
            /* Force full widget sync on the newly activated page */
            for (size_t k = 0; k < s_pages[i].binding_count; ++k) {
                s_pages[i].bindings[k].last_version = 0;
            }
            lv_screen_load(s_pages[i].screen);
            return EDGEHMI_OK;
        }
    }
    return EDGEHMI_ERR_NOT_FOUND;
}

void edgehmi_ui_sync_tick(void) {
    if (!s_bus || s_page_count == 0) return;

    /* Check bus-wide RS485 health for Fail-Safe banner */
    if (s_comm_banner) {
        bool comm_lost = false;
        for (size_t t = 0; t < s_bus->count; ++t) {
            edgehmi_quality_t q = TAG_QUAL_INIT;
            edgehmi_bus_get(s_bus, (int16_t)t, NULL, &q);
            if (q == TAG_QUAL_STALE || q == TAG_QUAL_BAD) {
                comm_lost = true;
                break;
            }
        }
        if (comm_lost) {
            lv_obj_remove_flag(s_comm_banner, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_comm_banner, LV_OBJ_FLAG_HIDDEN);
        }
    }

    ui_page_runtime_t *rt = &s_pages[s_active_page];
    for (size_t i = 0; i < rt->binding_count; ++i) {
        ui_binding_t *b = &rt->bindings[i];
        if (!b->meta) continue;

        if (b->meta->type == WIDGET_ALARM_LIST && s_hist && b->aux_label) {
            if (s_hist->next_alarm_id != s_last_alarm_seq) {
                s_last_alarm_seq = s_hist->next_alarm_id;
                char abuf[320] = {0};
                size_t pos = 0;
                size_t shown = s_hist->alarm_count > 4 ? 4 : s_hist->alarm_count;
                for (size_t k = 0; k < shown && pos < sizeof(abuf) - 48; ++k) {
                    size_t idx = (s_hist->alarm_head + EDGEHMI_MAX_ALARMS - 1u - k) % EDGEHMI_MAX_ALARMS;
                    const edgehmi_alarm_record_t *ar = &s_hist->alarms[idx];
                    pos += (size_t)snprintf(abuf + pos, sizeof(abuf) - pos, "%s%s %s",
                                            k > 0 ? "\n" : "",
                                            ar->acknowledged ? "[ACK]" : "[!]",
                                            ar->message);
                }
                lv_label_set_text(b->aux_label, pos > 0 ? abuf : "No active alarms");
            }
            continue;
        }

        if (b->meta->bind_tag_idx < 0) continue;

        double val = 0.0;
        edgehmi_quality_t qual = TAG_QUAL_INIT;
        uint32_t ver = 0;
        if (edgehmi_bus_get_snapshot(s_bus, b->meta->bind_tag_idx, &val, &qual, &ver) == EDGEHMI_OK &&
            ver != b->last_version) {
            b->last_version = ver;
            apply_binding_value(b, val, qual);
        }
    }
}
