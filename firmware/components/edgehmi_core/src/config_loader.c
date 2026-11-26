#include "config_loader.h"

#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_err(char *buf, size_t len, const char *msg) {
    if (!buf || len == 0) {
        return;
    }
    snprintf(buf, len, "%s", msg ? msg : "unknown error");
}

static void copy_str(char *dst, size_t cap, const char *src) {
    if (!dst || cap == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t n = strlen(src);
    if (n >= cap) {
        n = cap - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static edgehmi_tag_type_t parse_tag_type(const char *s, bool *ok) {
    *ok = true;
    if (!s) { *ok = false; return TAG_TYPE_U16; }
    if (strcmp(s, "bool") == 0) return TAG_TYPE_BOOL;
    if (strcmp(s, "u16") == 0)  return TAG_TYPE_U16;
    if (strcmp(s, "i16") == 0)  return TAG_TYPE_I16;
    if (strcmp(s, "u32") == 0)  return TAG_TYPE_U32;
    if (strcmp(s, "f32") == 0)  return TAG_TYPE_F32;
    *ok = false;
    return TAG_TYPE_U16;
}

static edgehmi_access_t parse_access(const char *s) {
    if (!s) return TAG_ACCESS_R;
    if (strcmp(s, "rw") == 0) return TAG_ACCESS_RW;
    if (strcmp(s, "w") == 0)  return TAG_ACCESS_W;
    return TAG_ACCESS_R;
}

static edgehmi_byte_order_t parse_byte_order(const char *s, bool *ok) {
    *ok = true;
    if (!s || strcmp(s, "ABCD") == 0) return BYTE_ORDER_ABCD;
    if (strcmp(s, "CDAB") == 0) return BYTE_ORDER_CDAB;
    if (strcmp(s, "BADC") == 0) return BYTE_ORDER_BADC;
    if (strcmp(s, "DCBA") == 0) return BYTE_ORDER_DCBA;
    *ok = false;
    return BYTE_ORDER_ABCD;
}

static edgehmi_err_t parse_rule_cond_item(const cJSON *obj,
                                          edgehmi_bus_t *bus,
                                          edgehmi_rule_cond_t *out,
                                          char *err_buf,
                                          size_t err_len) {
    const cJSON *jctag = cJSON_GetObjectItemCaseSensitive(obj, "tag");
    const cJSON *jop   = cJSON_GetObjectItemCaseSensitive(obj, "op");
    const cJSON *jval  = cJSON_GetObjectItemCaseSensitive(obj, "value");
    if (!cJSON_IsString(jctag) || !cJSON_IsString(jop) || !cJSON_IsNumber(jval)) {
        set_err(err_buf, err_len, "rule condition requires tag, op, value");
        return EDGEHMI_ERR_SCHEMA;
    }
    bool op_ok = false;
    edgehmi_rule_op_t op = RULE_OP_GT;
    if (strcmp(jop->valuestring, ">") == 0)       { op = RULE_OP_GT; op_ok = true; }
    else if (strcmp(jop->valuestring, ">=") == 0) { op = RULE_OP_GE; op_ok = true; }
    else if (strcmp(jop->valuestring, "<") == 0)  { op = RULE_OP_LT; op_ok = true; }
    else if (strcmp(jop->valuestring, "<=") == 0) { op = RULE_OP_LE; op_ok = true; }
    else if (strcmp(jop->valuestring, "==") == 0) { op = RULE_OP_EQ; op_ok = true; }
    else if (strcmp(jop->valuestring, "!=") == 0) { op = RULE_OP_NE; op_ok = true; }
    if (!op_ok) {
        set_err(err_buf, err_len, "invalid rule operator");
        return EDGEHMI_ERR_SCHEMA;
    }

    memset(out, 0, sizeof(*out));
    copy_str(out->tag, sizeof(out->tag), jctag->valuestring);
    out->tag_idx = edgehmi_bus_find(bus, out->tag);
    if (out->tag_idx < 0) {
        set_err(err_buf, err_len, "rule references unknown condition tag");
        return EDGEHMI_ERR_NOT_FOUND;
    }
    out->op = op;
    out->threshold = jval->valuedouble;
    const cJSON *jhyst = cJSON_GetObjectItemCaseSensitive(obj, "hysteresis");
    if (cJSON_IsNumber(jhyst) && jhyst->valuedouble >= 0.0) {
        out->hysteresis = jhyst->valuedouble;
    }
    return EDGEHMI_OK;
}

static edgehmi_widget_type_t parse_widget_type(const char *s, bool *ok) {
    *ok = true;
    if (!s) { *ok = false; return WIDGET_LABEL; }
    if (strcmp(s, "card") == 0)   return WIDGET_CARD;
    if (strcmp(s, "label") == 0)  return WIDGET_LABEL;
    if (strcmp(s, "value") == 0)  return WIDGET_VALUE;
    if (strcmp(s, "gauge") == 0)  return WIDGET_GAUGE;
    if (strcmp(s, "bar") == 0)    return WIDGET_BAR;
    if (strcmp(s, "button") == 0) return WIDGET_BUTTON;
    if (strcmp(s, "switch") == 0) return WIDGET_SWITCH;
    if (strcmp(s, "badge") == 0)  return WIDGET_BADGE;
    if (strcmp(s, "chart") == 0)  return WIDGET_CHART;
    *ok = false;
    return WIDGET_LABEL;
}

static uint32_t parse_hex_color(const char *s, uint32_t fallback) {
    if (!s || s[0] != '#' || strlen(s) != 7) {
        return fallback;
    }
    char *endp = NULL;
    unsigned long val = strtoul(s + 1, &endp, 16);
    if (!endp || *endp != '\0') {
        return fallback;
    }
    return (uint32_t)(val & 0xFFFFFFu);
}

edgehmi_err_t edgehmi_config_load_json(const char *json_str,
                                       edgehmi_config_t *out_cfg,
                                       edgehmi_bus_t *bus,
                                       char *err_buf,
                                       size_t err_buf_len) {
    if (!json_str || !out_cfg || !bus) {
        set_err(err_buf, err_buf_len, "null argument");
        return EDGEHMI_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_Parse(json_str);
    if (!root || !cJSON_IsObject(root)) {
        set_err(err_buf, err_buf_len, "invalid JSON root object");
        cJSON_Delete(root);
        return EDGEHMI_ERR_PARSE;
    }

    memset(out_cfg, 0, sizeof(*out_cfg));
    out_cfg->dark_theme = true;
    out_cfg->poll_interval_ms = 250;
    out_cfg->watchdog_timeout_s = 10;
    copy_str(out_cfg->engineer_pin, sizeof(out_cfg->engineer_pin), "1234");
    out_cfg->mb_port = 1;
    out_cfg->mb_baudrate = 9600;
    out_cfg->mb_tx_pin = 17;
    out_cfg->mb_rx_pin = 18;
    out_cfg->mb_de_pin = 16;
    out_cfg->mb_timeout_ms = 300;

    /* version */
    const cJSON *jver = cJSON_GetObjectItemCaseSensitive(root, "version");
    if (!cJSON_IsString(jver)) {
        set_err(err_buf, err_buf_len, "missing string field: version");
        cJSON_Delete(root);
        return EDGEHMI_ERR_SCHEMA;
    }
    copy_str(out_cfg->version, sizeof(out_cfg->version), jver->valuestring);

    /* device */
    const cJSON *jdev = cJSON_GetObjectItemCaseSensitive(root, "device");
    if (!cJSON_IsObject(jdev)) {
        set_err(err_buf, err_buf_len, "missing object field: device");
        cJSON_Delete(root);
        return EDGEHMI_ERR_SCHEMA;
    }
    const cJSON *jname = cJSON_GetObjectItemCaseSensitive(jdev, "name");
    const cJSON *jw = cJSON_GetObjectItemCaseSensitive(jdev, "width");
    const cJSON *jh = cJSON_GetObjectItemCaseSensitive(jdev, "height");
    if (!cJSON_IsString(jname) || !cJSON_IsNumber(jw) || !cJSON_IsNumber(jh)) {
        set_err(err_buf, err_buf_len, "device requires name, width, height");
        cJSON_Delete(root);
        return EDGEHMI_ERR_SCHEMA;
    }
    copy_str(out_cfg->device_name, sizeof(out_cfg->device_name), jname->valuestring);
    out_cfg->width = (uint16_t)jw->valueint;
    out_cfg->height = (uint16_t)jh->valueint;

    const cJSON *jtheme = cJSON_GetObjectItemCaseSensitive(jdev, "theme");
    if (cJSON_IsString(jtheme) && strcmp(jtheme->valuestring, "light") == 0) {
        out_cfg->dark_theme = false;
    }
    const cJSON *jpoll = cJSON_GetObjectItemCaseSensitive(jdev, "poll_interval_ms");
    if (cJSON_IsNumber(jpoll) && jpoll->valueint >= 50) {
        out_cfg->poll_interval_ms = (uint32_t)jpoll->valueint;
    }
    const cJSON *jwdt = cJSON_GetObjectItemCaseSensitive(jdev, "watchdog_timeout_s");
    if (cJSON_IsNumber(jwdt) && jwdt->valueint >= 3) {
        out_cfg->watchdog_timeout_s = (uint32_t)jwdt->valueint;
    }
    const cJSON *jpin = cJSON_GetObjectItemCaseSensitive(jdev, "engineer_pin");
    if (cJSON_IsString(jpin) && jpin->valuestring[0] != '\0') {
        copy_str(out_cfg->engineer_pin, sizeof(out_cfg->engineer_pin), jpin->valuestring);
    }
    const cJSON *jtok = cJSON_GetObjectItemCaseSensitive(jdev, "api_token");
    if (cJSON_IsString(jtok) && jtok->valuestring[0] != '\0') {
        copy_str(out_cfg->api_token, sizeof(out_cfg->api_token), jtok->valuestring);
    }

    /* modbus */
    const cJSON *jmb = cJSON_GetObjectItemCaseSensitive(root, "modbus");
    if (!cJSON_IsObject(jmb)) {
        set_err(err_buf, err_buf_len, "missing object field: modbus");
        cJSON_Delete(root);
        return EDGEHMI_ERR_SCHEMA;
    }
    const cJSON *jbaud = cJSON_GetObjectItemCaseSensitive(jmb, "baudrate");
    if (cJSON_IsNumber(jbaud)) out_cfg->mb_baudrate = (uint32_t)jbaud->valueint;
    const cJSON *jport = cJSON_GetObjectItemCaseSensitive(jmb, "port");
    if (cJSON_IsNumber(jport)) out_cfg->mb_port = (uint8_t)jport->valueint;
    const cJSON *jtx = cJSON_GetObjectItemCaseSensitive(jmb, "tx_pin");
    if (cJSON_IsNumber(jtx)) out_cfg->mb_tx_pin = (int8_t)jtx->valueint;
    const cJSON *jrx = cJSON_GetObjectItemCaseSensitive(jmb, "rx_pin");
    if (cJSON_IsNumber(jrx)) out_cfg->mb_rx_pin = (int8_t)jrx->valueint;
    const cJSON *jde = cJSON_GetObjectItemCaseSensitive(jmb, "de_pin");
    if (cJSON_IsNumber(jde)) out_cfg->mb_de_pin = (int8_t)jde->valueint;
    const cJSON *jtmo = cJSON_GetObjectItemCaseSensitive(jmb, "timeout_ms");
    if (cJSON_IsNumber(jtmo)) out_cfg->mb_timeout_ms = (uint32_t)jtmo->valueint;

    const cJSON *jpts = cJSON_GetObjectItemCaseSensitive(jmb, "points");
    if (!cJSON_IsArray(jpts)) {
        set_err(err_buf, err_buf_len, "modbus.points must be an array");
        cJSON_Delete(root);
        return EDGEHMI_ERR_SCHEMA;
    }

    const cJSON *jpt = NULL;
    cJSON_ArrayForEach(jpt, jpts) {
        if (out_cfg->point_count >= EDGEHMI_MAX_MODBUS_POINTS) {
            set_err(err_buf, err_buf_len, "exceeded max modbus points");
            cJSON_Delete(root);
            return EDGEHMI_ERR_FULL;
        }
        const cJSON *jtag = cJSON_GetObjectItemCaseSensitive(jpt, "tag");
        const cJSON *jslave = cJSON_GetObjectItemCaseSensitive(jpt, "slave");
        const cJSON *jfc = cJSON_GetObjectItemCaseSensitive(jpt, "fc");
        const cJSON *jaddr = cJSON_GetObjectItemCaseSensitive(jpt, "addr");
        const cJSON *jtype = cJSON_GetObjectItemCaseSensitive(jpt, "type");
        if (!cJSON_IsString(jtag) || !cJSON_IsNumber(jslave) ||
            !cJSON_IsNumber(jfc) || !cJSON_IsNumber(jaddr) || !cJSON_IsString(jtype)) {
            set_err(err_buf, err_buf_len, "invalid modbus point entry");
            cJSON_Delete(root);
            return EDGEHMI_ERR_SCHEMA;
        }

        bool ok = false;
        edgehmi_tag_type_t tt = parse_tag_type(jtype->valuestring, &ok);
        if (!ok) {
            set_err(err_buf, err_buf_len, "unknown modbus point type");
            cJSON_Delete(root);
            return EDGEHMI_ERR_SCHEMA;
        }

        edgehmi_mb_point_t *pt = &out_cfg->points[out_cfg->point_count];
        memset(pt, 0, sizeof(*pt));
        copy_str(pt->tag_name, sizeof(pt->tag_name), jtag->valuestring);
        pt->slave_id = (uint8_t)jslave->valueint;
        pt->func_code = (uint8_t)jfc->valueint;
        pt->reg_addr = (uint16_t)jaddr->valueint;
        pt->type = tt;

        const cJSON *jbo = cJSON_GetObjectItemCaseSensitive(jpt, "byte_order");
        if (cJSON_IsString(jbo)) {
            bool bo_ok = false;
            pt->byte_order = parse_byte_order(jbo->valuestring, &bo_ok);
            if (!bo_ok) {
                set_err(err_buf, err_buf_len, "invalid modbus point byte_order");
                cJSON_Delete(root);
                return EDGEHMI_ERR_SCHEMA;
            }
        } else {
            pt->byte_order = BYTE_ORDER_ABCD;
        }

        const cJSON *jrw = cJSON_GetObjectItemCaseSensitive(jpt, "rw");
        pt->access = parse_access(cJSON_IsString(jrw) ? jrw->valuestring : "r");

        const cJSON *jscale = cJSON_GetObjectItemCaseSensitive(jpt, "scale");
        pt->scale = cJSON_IsNumber(jscale) ? jscale->valuedouble : 1.0;

        const cJSON *joff = cJSON_GetObjectItemCaseSensitive(jpt, "offset");
        pt->offset = cJSON_IsNumber(joff) ? joff->valuedouble : 0.0;

        const cJSON *jdef = cJSON_GetObjectItemCaseSensitive(jpt, "default");
        pt->default_val = cJSON_IsNumber(jdef) ? jdef->valuedouble : 0.0;

        const cJSON *jret = cJSON_GetObjectItemCaseSensitive(jpt, "retentive");
        pt->retentive = cJSON_IsBool(jret) && cJSON_IsTrue(jret);

        const cJSON *junit = cJSON_GetObjectItemCaseSensitive(jpt, "unit");
        if (cJSON_IsString(junit)) {
            copy_str(pt->unit, sizeof(pt->unit), junit->valuestring);
        }

        edgehmi_err_t rc = edgehmi_bus_register(
            bus, pt->tag_name, pt->type, pt->access, pt->unit, pt->default_val, &pt->tag_idx);
        if (rc != EDGEHMI_OK) {
            set_err(err_buf, err_buf_len, "failed to register tag on data bus");
            cJSON_Delete(root);
            return rc;
        }
        out_cfg->point_count++;
    }

    /* rules */
    const cJSON *jrules = cJSON_GetObjectItemCaseSensitive(root, "rules");
    if (cJSON_IsArray(jrules)) {
        const cJSON *jr = NULL;
        cJSON_ArrayForEach(jr, jrules) {
            if (out_cfg->rule_count >= EDGEHMI_MAX_RULES) {
                set_err(err_buf, err_buf_len, "exceeded max rules");
                cJSON_Delete(root);
                return EDGEHMI_ERR_FULL;
            }
            const cJSON *jid = cJSON_GetObjectItemCaseSensitive(jr, "id");
            const cJSON *jwhen = cJSON_GetObjectItemCaseSensitive(jr, "when");
            const cJSON *jthen = cJSON_GetObjectItemCaseSensitive(jr, "then");
            if (!cJSON_IsString(jid) || !cJSON_IsObject(jwhen) || !cJSON_IsObject(jthen)) {
                set_err(err_buf, err_buf_len, "rule requires id, when, then");
                cJSON_Delete(root);
                return EDGEHMI_ERR_SCHEMA;
            }

            edgehmi_rule_t *r = &out_cfg->rules[out_cfg->rule_count];
            memset(r, 0, sizeof(*r));
            r->target_tag_idx = -1;
            r->enabled = true;
            r->logic_mode = RULE_LOGIC_ALL;
            copy_str(r->id, sizeof(r->id), jid->valuestring);

            const cJSON *jdesc = cJSON_GetObjectItemCaseSensitive(jr, "desc");
            if (cJSON_IsString(jdesc)) copy_str(r->desc, sizeof(r->desc), jdesc->valuestring);
            const cJSON *jen = cJSON_GetObjectItemCaseSensitive(jr, "enabled");
            if (cJSON_IsBool(jen)) r->enabled = cJSON_IsTrue(jen);
            const cJSON *jenf = cJSON_GetObjectItemCaseSensitive(jr, "enforce");
            if (cJSON_IsBool(jenf)) r->enforce = cJSON_IsTrue(jenf);
            const cJSON *jthen_enf = cJSON_GetObjectItemCaseSensitive(jthen, "enforce");
            if (cJSON_IsBool(jthen_enf) && cJSON_IsTrue(jthen_enf)) r->enforce = true;

            const cJSON *jdeb = cJSON_GetObjectItemCaseSensitive(jwhen, "debounce_ms");
            if (cJSON_IsNumber(jdeb) && jdeb->valueint >= 0) {
                r->debounce_ms = (uint32_t)jdeb->valueint;
            }

            const cJSON *jcomm = cJSON_GetObjectItemCaseSensitive(jwhen, "on_comm_loss");
            const cJSON *jall  = cJSON_GetObjectItemCaseSensitive(jwhen, "all");
            const cJSON *jany  = cJSON_GetObjectItemCaseSensitive(jwhen, "any");

            if (cJSON_IsBool(jcomm) && cJSON_IsTrue(jcomm)) {
                r->logic_mode = RULE_LOGIC_COMM_LOSS;
                const cJSON *jctag = cJSON_GetObjectItemCaseSensitive(jwhen, "tag");
                if (cJSON_IsString(jctag) && jctag->valuestring[0] != '\0') {
                    copy_str(r->conds[0].tag, sizeof(r->conds[0].tag), jctag->valuestring);
                    r->conds[0].tag_idx = edgehmi_bus_find(bus, r->conds[0].tag);
                    if (r->conds[0].tag_idx < 0) {
                        set_err(err_buf, err_buf_len, "on_comm_loss references unknown tag");
                        cJSON_Delete(root);
                        return EDGEHMI_ERR_NOT_FOUND;
                    }
                    r->cond_count = 1;
                }
            } else if (cJSON_IsArray(jall) || cJSON_IsArray(jany)) {
                const cJSON *arr = cJSON_IsArray(jall) ? jall : jany;
                r->logic_mode = cJSON_IsArray(jall) ? RULE_LOGIC_ALL : RULE_LOGIC_ANY;
                if (cJSON_GetArraySize(arr) == 0 || cJSON_GetArraySize(arr) > EDGEHMI_MAX_RULE_CONDS) {
                    set_err(err_buf, err_buf_len, "compound rule must have 1..4 conditions");
                    cJSON_Delete(root);
                    return EDGEHMI_ERR_SCHEMA;
                }
                const cJSON *ci = NULL;
                cJSON_ArrayForEach(ci, arr) {
                    edgehmi_err_t crc = parse_rule_cond_item(
                        ci, bus, &r->conds[r->cond_count], err_buf, err_buf_len);
                    if (crc != EDGEHMI_OK) {
                        cJSON_Delete(root);
                        return crc;
                    }
                    r->cond_count++;
                }
            } else {
                r->logic_mode = RULE_LOGIC_ALL;
                edgehmi_err_t crc = parse_rule_cond_item(
                    jwhen, bus, &r->conds[0], err_buf, err_buf_len);
                if (crc != EDGEHMI_OK) {
                    cJSON_Delete(root);
                    return crc;
                }
                r->cond_count = 1;
            }

            const cJSON *jact = cJSON_GetObjectItemCaseSensitive(jthen, "action");
            if (!cJSON_IsString(jact)) {
                set_err(err_buf, err_buf_len, "rule.then requires action");
                cJSON_Delete(root);
                return EDGEHMI_ERR_SCHEMA;
            }
            if (strcmp(jact->valuestring, "set_tag") == 0) {
                r->action = RULE_ACT_SET_TAG;
                const cJSON *jtt = cJSON_GetObjectItemCaseSensitive(jthen, "target_tag");
                const cJSON *jav = cJSON_GetObjectItemCaseSensitive(jthen, "value");
                if (!cJSON_IsString(jtt) || !cJSON_IsNumber(jav)) {
                    set_err(err_buf, err_buf_len, "set_tag requires target_tag and value");
                    cJSON_Delete(root);
                    return EDGEHMI_ERR_SCHEMA;
                }
                copy_str(r->target_tag, sizeof(r->target_tag), jtt->valuestring);
                r->target_tag_idx = edgehmi_bus_find(bus, r->target_tag);
                if (r->target_tag_idx < 0) {
                    set_err(err_buf, err_buf_len, "rule references unknown target_tag");
                    cJSON_Delete(root);
                    return EDGEHMI_ERR_NOT_FOUND;
                }
                r->action_value = jav->valuedouble;
            } else if (strcmp(jact->valuestring, "alarm") == 0) {
                r->action = RULE_ACT_ALARM;
                const cJSON *jlvl = cJSON_GetObjectItemCaseSensitive(jthen, "level");
                r->alarm_level = ALARM_WARN;
                if (cJSON_IsString(jlvl)) {
                    if (strcmp(jlvl->valuestring, "info") == 0) r->alarm_level = ALARM_INFO;
                    else if (strcmp(jlvl->valuestring, "critical") == 0) r->alarm_level = ALARM_CRITICAL;
                }
                const cJSON *jmsg = cJSON_GetObjectItemCaseSensitive(jthen, "message");
                if (cJSON_IsString(jmsg)) {
                    copy_str(r->alarm_msg, sizeof(r->alarm_msg), jmsg->valuestring);
                }
            } else {
                set_err(err_buf, err_buf_len, "unknown rule action");
                cJSON_Delete(root);
                return EDGEHMI_ERR_SCHEMA;
            }

            out_cfg->rule_count++;
        }
    }

    /* pages */
    const cJSON *jpages = cJSON_GetObjectItemCaseSensitive(root, "pages");
    if (!cJSON_IsArray(jpages) || cJSON_GetArraySize(jpages) == 0) {
        set_err(err_buf, err_buf_len, "pages must be a non-empty array");
        cJSON_Delete(root);
        return EDGEHMI_ERR_SCHEMA;
    }

    const cJSON *jpg = NULL;
    cJSON_ArrayForEach(jpg, jpages) {
        if (out_cfg->page_count >= EDGEHMI_MAX_PAGES) {
            set_err(err_buf, err_buf_len, "exceeded max pages");
            cJSON_Delete(root);
            return EDGEHMI_ERR_FULL;
        }
        const cJSON *jpid = cJSON_GetObjectItemCaseSensitive(jpg, "id");
        const cJSON *jptitle = cJSON_GetObjectItemCaseSensitive(jpg, "title");
        const cJSON *jwidgets = cJSON_GetObjectItemCaseSensitive(jpg, "widgets");
        if (!cJSON_IsString(jpid) || !cJSON_IsString(jptitle) || !cJSON_IsArray(jwidgets)) {
            set_err(err_buf, err_buf_len, "page requires id, title, widgets");
            cJSON_Delete(root);
            return EDGEHMI_ERR_SCHEMA;
        }

        edgehmi_page_t *pg = &out_cfg->pages[out_cfg->page_count];
        memset(pg, 0, sizeof(*pg));
        copy_str(pg->id, sizeof(pg->id), jpid->valuestring);
        copy_str(pg->title, sizeof(pg->title), jptitle->valuestring);

        const cJSON *jw_item = NULL;
        cJSON_ArrayForEach(jw_item, jwidgets) {
            if (pg->widget_count >= EDGEHMI_MAX_WIDGETS_PER_PG) {
                set_err(err_buf, err_buf_len, "exceeded max widgets per page");
                cJSON_Delete(root);
                return EDGEHMI_ERR_FULL;
            }
            const cJSON *jwid = cJSON_GetObjectItemCaseSensitive(jw_item, "id");
            const cJSON *jwtype = cJSON_GetObjectItemCaseSensitive(jw_item, "type");
            const cJSON *jx = cJSON_GetObjectItemCaseSensitive(jw_item, "x");
            const cJSON *jy = cJSON_GetObjectItemCaseSensitive(jw_item, "y");
            const cJSON *jww = cJSON_GetObjectItemCaseSensitive(jw_item, "w");
            const cJSON *jwh = cJSON_GetObjectItemCaseSensitive(jw_item, "h");
            if (!cJSON_IsString(jwid) || !cJSON_IsString(jwtype) ||
                !cJSON_IsNumber(jx) || !cJSON_IsNumber(jy) ||
                !cJSON_IsNumber(jww) || !cJSON_IsNumber(jwh)) {
                set_err(err_buf, err_buf_len, "widget missing required geometry/type fields");
                cJSON_Delete(root);
                return EDGEHMI_ERR_SCHEMA;
            }

            bool wt_ok = false;
            edgehmi_widget_type_t wt = parse_widget_type(jwtype->valuestring, &wt_ok);
            if (!wt_ok) {
                set_err(err_buf, err_buf_len, "unknown widget type");
                cJSON_Delete(root);
                return EDGEHMI_ERR_SCHEMA;
            }

            edgehmi_widget_t *w = &pg->widgets[pg->widget_count];
            memset(w, 0, sizeof(*w));
            w->bind_tag_idx = -1;
            w->type = wt;
            copy_str(w->id, sizeof(w->id), jwid->valuestring);
            w->x = (int16_t)jx->valueint;
            w->y = (int16_t)jy->valueint;
            w->w = (int16_t)jww->valueint;
            w->h = (int16_t)jwh->valueint;
            w->max_val = 100.0;
            w->decimals = 1;
            w->color_rgb = 0x2DD4BFu;

            const cJSON *jtxt = cJSON_GetObjectItemCaseSensitive(jw_item, "text");
            if (cJSON_IsString(jtxt)) copy_str(w->text, sizeof(w->text), jtxt->valuestring);

            const cJSON *jbind = cJSON_GetObjectItemCaseSensitive(jw_item, "bind");
            if (cJSON_IsString(jbind) && jbind->valuestring[0] != '\0') {
                copy_str(w->bind_tag, sizeof(w->bind_tag), jbind->valuestring);
                w->bind_tag_idx = edgehmi_bus_find(bus, w->bind_tag);
                if (w->bind_tag_idx < 0) {
                    set_err(err_buf, err_buf_len, "widget binds to unknown tag");
                    cJSON_Delete(root);
                    return EDGEHMI_ERR_NOT_FOUND;
                }
            }

            const cJSON *junit = cJSON_GetObjectItemCaseSensitive(jw_item, "unit");
            if (cJSON_IsString(junit)) copy_str(w->unit, sizeof(w->unit), junit->valuestring);
            const cJSON *jmin = cJSON_GetObjectItemCaseSensitive(jw_item, "min");
            if (cJSON_IsNumber(jmin)) w->min_val = jmin->valuedouble;
            const cJSON *jmax = cJSON_GetObjectItemCaseSensitive(jw_item, "max");
            if (cJSON_IsNumber(jmax)) w->max_val = jmax->valuedouble;
            const cJSON *jdec = cJSON_GetObjectItemCaseSensitive(jw_item, "decimals");
            if (cJSON_IsNumber(jdec) && jdec->valueint >= 0) w->decimals = (uint8_t)jdec->valueint;
            const cJSON *jcol = cJSON_GetObjectItemCaseSensitive(jw_item, "color");
            if (cJSON_IsString(jcol)) w->color_rgb = parse_hex_color(jcol->valuestring, 0x2DD4BFu);
            const cJSON *jwv = cJSON_GetObjectItemCaseSensitive(jw_item, "write_value");
            if (cJSON_IsNumber(jwv)) {
                w->has_write_value = true;
                w->write_value = jwv->valuedouble;
            }
            const cJSON *jnav = cJSON_GetObjectItemCaseSensitive(jw_item, "nav_page");
            if (cJSON_IsString(jnav)) copy_str(w->nav_page, sizeof(w->nav_page), jnav->valuestring);
            const cJSON *jtt = cJSON_GetObjectItemCaseSensitive(jw_item, "true_text");
            if (cJSON_IsString(jtt)) copy_str(w->true_text, sizeof(w->true_text), jtt->valuestring);
            const cJSON *jft = cJSON_GetObjectItemCaseSensitive(jw_item, "false_text");
            if (cJSON_IsString(jft)) copy_str(w->false_text, sizeof(w->false_text), jft->valuestring);

            const cJSON *jconf = cJSON_GetObjectItemCaseSensitive(jw_item, "confirm");
            w->confirm = cJSON_IsBool(jconf) && cJSON_IsTrue(jconf);
            const cJSON *jctxt = cJSON_GetObjectItemCaseSensitive(jw_item, "confirm_text");
            if (cJSON_IsString(jctxt)) copy_str(w->confirm_text, sizeof(w->confirm_text), jctxt->valuestring);
            const cJSON *jpinreq = cJSON_GetObjectItemCaseSensitive(jw_item, "pin_required");
            w->pin_required = cJSON_IsBool(jpinreq) && cJSON_IsTrue(jpinreq);

            pg->widget_count++;
        }

        out_cfg->page_count++;
    }

    cJSON_Delete(root);
    return EDGEHMI_OK;
}
