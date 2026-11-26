#include "config_loader.h"

#include "cJSON.h"

#include <math.h>
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
    edgehmi_copy_str(dst, cap, src);
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

static bool parse_rule_op_str(const char *s, edgehmi_rule_op_t *out) {
    if (!s || !out) return false;
    if (strcmp(s, ">") == 0)  { *out = RULE_OP_GT; return true; }
    if (strcmp(s, ">=") == 0) { *out = RULE_OP_GE; return true; }
    if (strcmp(s, "<") == 0)  { *out = RULE_OP_LT; return true; }
    if (strcmp(s, "<=") == 0) { *out = RULE_OP_LE; return true; }
    if (strcmp(s, "==") == 0) { *out = RULE_OP_EQ; return true; }
    if (strcmp(s, "!=") == 0) { *out = RULE_OP_NE; return true; }
    return false;
}

static bool eval_simple_op(edgehmi_rule_op_t op, double val, double thr) {
    switch (op) {
        case RULE_OP_GT: return val > thr;
        case RULE_OP_GE: return val >= thr;
        case RULE_OP_LT: return val < thr;
        case RULE_OP_LE: return val <= thr;
        case RULE_OP_EQ: return fabs(val - thr) < 1e-6;
        case RULE_OP_NE: return fabs(val - thr) >= 1e-6;
    }
    return false;
}

uint32_t edgehmi_widget_eval_color(const edgehmi_widget_t *w, double val) {
    if (!w) return 0x2DD4BFu;
    uint32_t col = w->color_rgb;
    for (size_t i = 0; i < w->color_rule_count; ++i) {
        if (eval_simple_op(w->color_rules[i].op, val, w->color_rules[i].threshold)) {
            col = w->color_rules[i].color_rgb;
        }
    }
    return col;
}

bool edgehmi_widget_eval_visible(const edgehmi_widget_t *w, edgehmi_bus_t *bus) {
    if (!w || !w->has_vis_rule || !bus || w->vis_tag_idx < 0) {
        return true;
    }
    double v = 0.0;
    if (edgehmi_bus_get(bus, w->vis_tag_idx, &v, NULL) != EDGEHMI_OK) {
        return true;
    }
    return eval_simple_op(w->vis_op, v, w->vis_threshold);
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
    if (strcmp(s, "card") == 0)       return WIDGET_CARD;
    if (strcmp(s, "label") == 0)      return WIDGET_LABEL;
    if (strcmp(s, "value") == 0)      return WIDGET_VALUE;
    if (strcmp(s, "gauge") == 0)      return WIDGET_GAUGE;
    if (strcmp(s, "bar") == 0)        return WIDGET_BAR;
    if (strcmp(s, "button") == 0)     return WIDGET_BUTTON;
    if (strcmp(s, "switch") == 0)     return WIDGET_SWITCH;
    if (strcmp(s, "badge") == 0)      return WIDGET_BADGE;
    if (strcmp(s, "chart") == 0)      return WIDGET_CHART;
    if (strcmp(s, "slider") == 0)     return WIDGET_SLIDER;
    if (strcmp(s, "spinbox") == 0)    return WIDGET_SPINBOX;
    if (strcmp(s, "alarm_list") == 0) return WIDGET_ALARM_LIST;
    if (strcmp(s, "qrcode") == 0)     return WIDGET_QRCODE;
    if (strcmp(s, "image") == 0)      return WIDGET_IMAGE;
    if (strcmp(s, "dropdown") == 0)   return WIDGET_DROPDOWN;
    if (strcmp(s, "led") == 0)        return WIDGET_LED;
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
    out_cfg->idle_dim_s = 0;
    out_cfg->idle_brightness_pct = 15;
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
    const cJSON *jdim = cJSON_GetObjectItemCaseSensitive(jdev, "idle_dim_s");
    if (cJSON_IsNumber(jdim) && jdim->valueint >= 0 && jdim->valueint <= 65535) {
        out_cfg->idle_dim_s = (uint16_t)jdim->valueint;
    }
    const cJSON *jib = cJSON_GetObjectItemCaseSensitive(jdev, "idle_brightness_pct");
    if (cJSON_IsNumber(jib) && jib->valueint >= 0 && jib->valueint <= 100) {
        out_cfg->idle_brightness_pct = (uint8_t)jib->valueint;
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

    /* optional: modbus_tcp (Southbound Client + Northbound SCADA Server Gateway) */
    out_cfg->modbus_tcp.port = 502;
    out_cfg->modbus_tcp.timeout_ms = 500;
    out_cfg->modbus_tcp.server_port = 502;
    const cJSON *jmbtcp = cJSON_GetObjectItemCaseSensitive(root, "modbus_tcp");
    if (cJSON_IsObject(jmbtcp)) {
        const cJSON *jen = cJSON_GetObjectItemCaseSensitive(jmbtcp, "enabled");
        out_cfg->modbus_tcp.enabled = cJSON_IsBool(jen) && cJSON_IsTrue(jen);
        const cJSON *jhost = cJSON_GetObjectItemCaseSensitive(jmbtcp, "host");
        if (cJSON_IsString(jhost)) copy_str(out_cfg->modbus_tcp.host, sizeof(out_cfg->modbus_tcp.host), jhost->valuestring);
        const cJSON *jport2 = cJSON_GetObjectItemCaseSensitive(jmbtcp, "port");
        if (cJSON_IsNumber(jport2) && jport2->valueint > 0) out_cfg->modbus_tcp.port = (uint16_t)jport2->valueint;
        const cJSON *jtmo2 = cJSON_GetObjectItemCaseSensitive(jmbtcp, "timeout_ms");
        if (cJSON_IsNumber(jtmo2) && jtmo2->valueint >= 50) out_cfg->modbus_tcp.timeout_ms = (uint32_t)jtmo2->valueint;
        const cJSON *jsen = cJSON_GetObjectItemCaseSensitive(jmbtcp, "server_enabled");
        out_cfg->modbus_tcp.server_enabled = cJSON_IsBool(jsen) && cJSON_IsTrue(jsen);
        const cJSON *jsport = cJSON_GetObjectItemCaseSensitive(jmbtcp, "server_port");
        if (cJSON_IsNumber(jsport) && jsport->valueint > 0) out_cfg->modbus_tcp.server_port = (uint16_t)jsport->valueint;
    }

    /* optional: local_io (native ESP32-S3 GPIO DI/DO/ADC/PWM mapped into unified Data Bus) */
    const cJSON *jlio = cJSON_GetObjectItemCaseSensitive(root, "local_io");
    if (cJSON_IsArray(jlio)) {
        const cJSON *ji = NULL;
        cJSON_ArrayForEach(ji, jlio) {
            if (out_cfg->local_io_count >= EDGEHMI_MAX_LOCAL_IO) {
                set_err(err_buf, err_buf_len, "exceeded max local_io entries");
                cJSON_Delete(root);
                return EDGEHMI_ERR_FULL;
            }
            const cJSON *jtag = cJSON_GetObjectItemCaseSensitive(ji, "tag");
            const cJSON *jpin = cJSON_GetObjectItemCaseSensitive(ji, "pin");
            const cJSON *jmode = cJSON_GetObjectItemCaseSensitive(ji, "mode");
            if (!cJSON_IsString(jtag) || !cJSON_IsNumber(jpin) || !cJSON_IsString(jmode)) {
                set_err(err_buf, err_buf_len, "local_io entry requires tag, pin, mode");
                cJSON_Delete(root);
                return EDGEHMI_ERR_SCHEMA;
            }
            edgehmi_local_io_t *io = &out_cfg->local_io[out_cfg->local_io_count];
            memset(io, 0, sizeof(*io));
            copy_str(io->tag_name, sizeof(io->tag_name), jtag->valuestring);
            io->gpio_pin = (int8_t)jpin->valueint;
            edgehmi_tag_type_t tt = TAG_TYPE_BOOL;
            edgehmi_access_t acc = TAG_ACCESS_R;
            if (strcmp(jmode->valuestring, "di") == 0)       { io->mode = LOCAL_IO_DI;  tt = TAG_TYPE_BOOL; acc = TAG_ACCESS_R; }
            else if (strcmp(jmode->valuestring, "do") == 0)  { io->mode = LOCAL_IO_DO;  tt = TAG_TYPE_BOOL; acc = TAG_ACCESS_RW; }
            else if (strcmp(jmode->valuestring, "adc") == 0) { io->mode = LOCAL_IO_ADC; tt = TAG_TYPE_F32;  acc = TAG_ACCESS_R; }
            else if (strcmp(jmode->valuestring, "pwm") == 0) { io->mode = LOCAL_IO_PWM; tt = TAG_TYPE_U16;  acc = TAG_ACCESS_RW; }
            else {
                set_err(err_buf, err_buf_len, "invalid local_io mode (di/do/adc/pwm)");
                cJSON_Delete(root);
                return EDGEHMI_ERR_SCHEMA;
            }
            const cJSON *jal = cJSON_GetObjectItemCaseSensitive(ji, "active_low");
            io->active_low = cJSON_IsBool(jal) && cJSON_IsTrue(jal);
            const cJSON *jsc = cJSON_GetObjectItemCaseSensitive(ji, "scale");
            io->scale = cJSON_IsNumber(jsc) ? jsc->valuedouble : 1.0;
            const cJSON *jof = cJSON_GetObjectItemCaseSensitive(ji, "offset");
            io->offset = cJSON_IsNumber(jof) ? jof->valuedouble : 0.0;
            const cJSON *jdf = cJSON_GetObjectItemCaseSensitive(ji, "default");
            io->default_val = cJSON_IsNumber(jdf) ? jdf->valuedouble : 0.0;
            const cJSON *jrt = cJSON_GetObjectItemCaseSensitive(ji, "retentive");
            io->retentive = cJSON_IsBool(jrt) && cJSON_IsTrue(jrt);
            const cJSON *jun = cJSON_GetObjectItemCaseSensitive(ji, "unit");
            if (cJSON_IsString(jun)) copy_str(io->unit, sizeof(io->unit), jun->valuestring);

            edgehmi_err_t rc = edgehmi_bus_register(
                bus, io->tag_name, tt, acc, io->unit, io->default_val, &io->tag_idx);
            if (rc != EDGEHMI_OK) {
                set_err(err_buf, err_buf_len, "failed to register local_io tag");
                cJSON_Delete(root);
                return rc;
            }
            out_cfg->local_io_count++;
        }
    }

    /* optional: can_bus (ESP32-S3 native TWAI CAN 2.0B for BMS / servo drives) */
    out_cfg->can_bus.bitrate = 500000;
    out_cfg->can_bus.tx_pin = 20;
    out_cfg->can_bus.rx_pin = 19;
    const cJSON *jcan = cJSON_GetObjectItemCaseSensitive(root, "can_bus");
    if (cJSON_IsObject(jcan)) {
        const cJSON *jen = cJSON_GetObjectItemCaseSensitive(jcan, "enabled");
        out_cfg->can_bus.enabled = cJSON_IsBool(jen) && cJSON_IsTrue(jen);
        const cJSON *jbr = cJSON_GetObjectItemCaseSensitive(jcan, "bitrate");
        if (cJSON_IsNumber(jbr)) out_cfg->can_bus.bitrate = (uint32_t)jbr->valueint;
        const cJSON *jctx = cJSON_GetObjectItemCaseSensitive(jcan, "tx_pin");
        if (cJSON_IsNumber(jctx)) out_cfg->can_bus.tx_pin = (int8_t)jctx->valueint;
        const cJSON *jcrx = cJSON_GetObjectItemCaseSensitive(jcan, "rx_pin");
        if (cJSON_IsNumber(jcrx)) out_cfg->can_bus.rx_pin = (int8_t)jcrx->valueint;

        const cJSON *jsigs = cJSON_GetObjectItemCaseSensitive(jcan, "signals");
        if (cJSON_IsArray(jsigs)) {
            const cJSON *js = NULL;
            cJSON_ArrayForEach(js, jsigs) {
                if (out_cfg->can_bus.signal_count >= EDGEHMI_MAX_CAN_SIGNALS) {
                    set_err(err_buf, err_buf_len, "exceeded max can_bus signals");
                    cJSON_Delete(root);
                    return EDGEHMI_ERR_FULL;
                }
                const cJSON *jtag = cJSON_GetObjectItemCaseSensitive(js, "tag");
                const cJSON *jcid = cJSON_GetObjectItemCaseSensitive(js, "can_id");
                const cJSON *jsb  = cJSON_GetObjectItemCaseSensitive(js, "start_bit");
                const cJSON *jbl  = cJSON_GetObjectItemCaseSensitive(js, "bit_len");
                if (!cJSON_IsString(jtag) || !cJSON_IsNumber(jcid) ||
                    !cJSON_IsNumber(jsb) || !cJSON_IsNumber(jbl)) {
                    set_err(err_buf, err_buf_len, "can_bus signal requires tag, can_id, start_bit, bit_len");
                    cJSON_Delete(root);
                    return EDGEHMI_ERR_SCHEMA;
                }
                edgehmi_can_signal_t *sig = &out_cfg->can_bus.signals[out_cfg->can_bus.signal_count];
                memset(sig, 0, sizeof(*sig));
                copy_str(sig->tag_name, sizeof(sig->tag_name), jtag->valuestring);
                sig->can_id = (uint32_t)jcid->valuedouble;
                const cJSON *jext = cJSON_GetObjectItemCaseSensitive(js, "ext_id");
                sig->ext_id = cJSON_IsBool(jext) && cJSON_IsTrue(jext);
                sig->start_bit = (uint8_t)jsb->valueint;
                sig->bit_len = (uint8_t)jbl->valueint;
                const cJSON *jsgn = cJSON_GetObjectItemCaseSensitive(js, "signed");
                sig->is_signed = cJSON_IsBool(jsgn) && cJSON_IsTrue(jsgn);
                const cJSON *jbe = cJSON_GetObjectItemCaseSensitive(js, "big_endian");
                sig->big_endian = cJSON_IsBool(jbe) && cJSON_IsTrue(jbe);
                const cJSON *jsc = cJSON_GetObjectItemCaseSensitive(js, "scale");
                sig->scale = cJSON_IsNumber(jsc) ? jsc->valuedouble : 1.0;
                const cJSON *jof = cJSON_GetObjectItemCaseSensitive(js, "offset");
                sig->offset = cJSON_IsNumber(jof) ? jof->valuedouble : 0.0;
                const cJSON *jdf = cJSON_GetObjectItemCaseSensitive(js, "default");
                sig->default_val = cJSON_IsNumber(jdf) ? jdf->valuedouble : 0.0;
                const cJSON *jun = cJSON_GetObjectItemCaseSensitive(js, "unit");
                if (cJSON_IsString(jun)) copy_str(sig->unit, sizeof(sig->unit), jun->valuestring);

                edgehmi_err_t rc = edgehmi_bus_register(
                    bus, sig->tag_name, TAG_TYPE_F32, TAG_ACCESS_R, sig->unit, sig->default_val, &sig->tag_idx);
                if (rc != EDGEHMI_OK) {
                    set_err(err_buf, err_buf_len, "failed to register can_bus signal tag");
                    cJSON_Delete(root);
                    return rc;
                }
                out_cfg->can_bus.signal_count++;
            }
        }
    }

    /* optional: calc_tags (derived virtual tags: add/sub/mul/div/avg/integrate) */
    const cJSON *jcalcs = cJSON_GetObjectItemCaseSensitive(root, "calc_tags");
    if (cJSON_IsArray(jcalcs)) {
        const cJSON *jc = NULL;
        cJSON_ArrayForEach(jc, jcalcs) {
            if (out_cfg->calc_tag_count >= EDGEHMI_MAX_CALC_TAGS) {
                set_err(err_buf, err_buf_len, "exceeded max calc_tags");
                cJSON_Delete(root);
                return EDGEHMI_ERR_FULL;
            }
            const cJSON *jtag = cJSON_GetObjectItemCaseSensitive(jc, "tag");
            const cJSON *jop  = cJSON_GetObjectItemCaseSensitive(jc, "op");
            const cJSON *jsa  = cJSON_GetObjectItemCaseSensitive(jc, "src_a");
            if (!cJSON_IsString(jtag) || !cJSON_IsString(jop) || !cJSON_IsString(jsa)) {
                set_err(err_buf, err_buf_len, "calc_tag requires tag, op, src_a");
                cJSON_Delete(root);
                return EDGEHMI_ERR_SCHEMA;
            }
            edgehmi_calc_tag_t *ct = &out_cfg->calc_tags[out_cfg->calc_tag_count];
            memset(ct, 0, sizeof(*ct));
            ct->tag_idx = -1;
            ct->src_a_idx = -1;
            ct->src_b_idx = -1;
            copy_str(ct->tag_name, sizeof(ct->tag_name), jtag->valuestring);
            copy_str(ct->src_a, sizeof(ct->src_a), jsa->valuestring);
            if (strcmp(jop->valuestring, "add") == 0)            ct->op = CALC_OP_ADD;
            else if (strcmp(jop->valuestring, "sub") == 0)       ct->op = CALC_OP_SUB;
            else if (strcmp(jop->valuestring, "mul") == 0)       ct->op = CALC_OP_MUL;
            else if (strcmp(jop->valuestring, "div") == 0)       ct->op = CALC_OP_DIV;
            else if (strcmp(jop->valuestring, "avg") == 0)       ct->op = CALC_OP_AVG;
            else if (strcmp(jop->valuestring, "integrate") == 0) ct->op = CALC_OP_INTEGRATE;
            else {
                set_err(err_buf, err_buf_len, "unknown calc_tag op");
                cJSON_Delete(root);
                return EDGEHMI_ERR_SCHEMA;
            }
            const cJSON *jsb = cJSON_GetObjectItemCaseSensitive(jc, "src_b");
            if (cJSON_IsString(jsb)) copy_str(ct->src_b, sizeof(ct->src_b), jsb->valuestring);
            const cJSON *jsc = cJSON_GetObjectItemCaseSensitive(jc, "scale");
            ct->scale = cJSON_IsNumber(jsc) ? jsc->valuedouble : 1.0;
            const cJSON *jof = cJSON_GetObjectItemCaseSensitive(jc, "offset");
            ct->offset = cJSON_IsNumber(jof) ? jof->valuedouble : 0.0;
            const cJSON *jun = cJSON_GetObjectItemCaseSensitive(jc, "unit");
            if (cJSON_IsString(jun)) copy_str(ct->unit, sizeof(ct->unit), jun->valuestring);

            ct->src_a_idx = edgehmi_bus_find(bus, ct->src_a);
            if (ct->src_a_idx < 0) {
                set_err(err_buf, err_buf_len, "calc_tag src_a unknown");
                cJSON_Delete(root);
                return EDGEHMI_ERR_NOT_FOUND;
            }
            if (ct->op != CALC_OP_INTEGRATE) {
                ct->src_b_idx = edgehmi_bus_find(bus, ct->src_b);
                if (ct->src_b_idx < 0) {
                    set_err(err_buf, err_buf_len, "calc_tag src_b unknown");
                    cJSON_Delete(root);
                    return EDGEHMI_ERR_NOT_FOUND;
                }
            }
            edgehmi_err_t rc = edgehmi_bus_register(
                bus, ct->tag_name, TAG_TYPE_F32, TAG_ACCESS_R, ct->unit, 0.0, &ct->tag_idx);
            if (rc != EDGEHMI_OK) {
                set_err(err_buf, err_buf_len, "failed to register calc_tag");
                cJSON_Delete(root);
                return rc;
            }
            out_cfg->calc_tag_count++;
        }
    }

    /* optional: cloud (Core 0 MQTT telemetry & remote command) */
    out_cfg->cloud.publish_interval_ms = 5000;
    copy_str(out_cfg->cloud.topic_prefix, sizeof(out_cfg->cloud.topic_prefix), "edgehmi");
    const cJSON *jcloud = cJSON_GetObjectItemCaseSensitive(root, "cloud");
    if (cJSON_IsObject(jcloud)) {
        const cJSON *jen = cJSON_GetObjectItemCaseSensitive(jcloud, "enabled");
        out_cfg->cloud.enabled = cJSON_IsBool(jen) && cJSON_IsTrue(jen);
        const cJSON *juri = cJSON_GetObjectItemCaseSensitive(jcloud, "mqtt_uri");
        if (cJSON_IsString(juri)) copy_str(out_cfg->cloud.mqtt_uri, sizeof(out_cfg->cloud.mqtt_uri), juri->valuestring);
        const cJSON *jcid = cJSON_GetObjectItemCaseSensitive(jcloud, "client_id");
        if (cJSON_IsString(jcid)) copy_str(out_cfg->cloud.client_id, sizeof(out_cfg->cloud.client_id), jcid->valuestring);
        const cJSON *jusr = cJSON_GetObjectItemCaseSensitive(jcloud, "username");
        if (cJSON_IsString(jusr)) copy_str(out_cfg->cloud.username, sizeof(out_cfg->cloud.username), jusr->valuestring);
        const cJSON *jpwd = cJSON_GetObjectItemCaseSensitive(jcloud, "password");
        if (cJSON_IsString(jpwd)) copy_str(out_cfg->cloud.password, sizeof(out_cfg->cloud.password), jpwd->valuestring);
        const cJSON *jtop = cJSON_GetObjectItemCaseSensitive(jcloud, "topic_prefix");
        if (cJSON_IsString(jtop)) copy_str(out_cfg->cloud.topic_prefix, sizeof(out_cfg->cloud.topic_prefix), jtop->valuestring);
        const cJSON *jint = cJSON_GetObjectItemCaseSensitive(jcloud, "publish_interval_ms");
        if (cJSON_IsNumber(jint) && jint->valueint >= 500) out_cfg->cloud.publish_interval_ms = (uint32_t)jint->valueint;
    }

    /* optional: network (Wi-Fi + SoftAP fallback + 4G Cat.1/Cat.4 & 5G RedCap/NR multi-link) */
    out_cfg->net.ap_fallback = true;
    out_cfg->net.failover_policy = NET_POLICY_WIFI_PRIMARY;
    out_cfg->net.cellular.generation = CELL_GEN_AUTO;
    out_cfg->net.cellular.iface = CELL_IFACE_UART_PPP;
    copy_str(out_cfg->net.cellular.apn, sizeof(out_cfg->net.cellular.apn), "cmnet");
    out_cfg->net.cellular.uart_port = 2;
    out_cfg->net.cellular.baudrate = 115200;
    out_cfg->net.cellular.tx_pin = -1;
    out_cfg->net.cellular.rx_pin = -1;
    out_cfg->net.cellular.rts_pin = -1;
    out_cfg->net.cellular.cts_pin = -1;
    out_cfg->net.cellular.pwrkey_pin = -1;
    out_cfg->net.cellular.reset_pin = -1;

    const cJSON *jnet = cJSON_GetObjectItemCaseSensitive(root, "network");
    if (cJSON_IsObject(jnet)) {
        const cJSON *jssid = cJSON_GetObjectItemCaseSensitive(jnet, "wifi_ssid");
        if (cJSON_IsString(jssid)) copy_str(out_cfg->net.ssid, sizeof(out_cfg->net.ssid), jssid->valuestring);
        const cJSON *jwpwd = cJSON_GetObjectItemCaseSensitive(jnet, "wifi_password");
        if (cJSON_IsString(jwpwd)) copy_str(out_cfg->net.password, sizeof(out_cfg->net.password), jwpwd->valuestring);
        const cJSON *jap = cJSON_GetObjectItemCaseSensitive(jnet, "ap_fallback");
        if (cJSON_IsBool(jap)) out_cfg->net.ap_fallback = cJSON_IsTrue(jap);

        const cJSON *jpol = cJSON_GetObjectItemCaseSensitive(jnet, "failover_policy");
        if (cJSON_IsString(jpol)) {
            if (strcmp(jpol->valuestring, "wifi_only") == 0) out_cfg->net.failover_policy = NET_POLICY_WIFI_ONLY;
            else if (strcmp(jpol->valuestring, "cellular_only") == 0) out_cfg->net.failover_policy = NET_POLICY_CELLULAR_ONLY;
            else if (strcmp(jpol->valuestring, "cellular_primary") == 0) out_cfg->net.failover_policy = NET_POLICY_CELLULAR_PRIMARY;
            else out_cfg->net.failover_policy = NET_POLICY_WIFI_PRIMARY;
        }

        /* Backwards-compatible legacy 4G flat keys */
        const cJSON *j4g = cJSON_GetObjectItemCaseSensitive(jnet, "modem_4g_enabled");
        if (cJSON_IsBool(j4g) && cJSON_IsTrue(j4g)) out_cfg->net.cellular.enabled = true;
        const cJSON *jmtx = cJSON_GetObjectItemCaseSensitive(jnet, "modem_tx_pin");
        if (cJSON_IsNumber(jmtx)) out_cfg->net.cellular.tx_pin = (int8_t)jmtx->valueint;
        const cJSON *jmrx = cJSON_GetObjectItemCaseSensitive(jnet, "modem_rx_pin");
        if (cJSON_IsNumber(jmrx)) out_cfg->net.cellular.rx_pin = (int8_t)jmrx->valueint;

        /* Structured 4G / 5G cellular configuration */
        const cJSON *jcell = cJSON_GetObjectItemCaseSensitive(jnet, "cellular");
        if (cJSON_IsObject(jcell)) {
            const cJSON *jcen = cJSON_GetObjectItemCaseSensitive(jcell, "enabled");
            if (cJSON_IsBool(jcen)) out_cfg->net.cellular.enabled = cJSON_IsTrue(jcen);
            const cJSON *jgen = cJSON_GetObjectItemCaseSensitive(jcell, "generation");
            if (cJSON_IsString(jgen)) {
                if (strcmp(jgen->valuestring, "4g") == 0) out_cfg->net.cellular.generation = CELL_GEN_4G;
                else if (strcmp(jgen->valuestring, "5g") == 0) out_cfg->net.cellular.generation = CELL_GEN_5G;
                else if (strcmp(jgen->valuestring, "5g_redcap") == 0) out_cfg->net.cellular.generation = CELL_GEN_5G_REDCAP;
                else out_cfg->net.cellular.generation = CELL_GEN_AUTO;
            }
            const cJSON *jif = cJSON_GetObjectItemCaseSensitive(jcell, "interface");
            if (cJSON_IsString(jif) && strcmp(jif->valuestring, "usb_ecm") == 0) {
                out_cfg->net.cellular.iface = CELL_IFACE_USB_ECM;
            }
            const cJSON *japn = cJSON_GetObjectItemCaseSensitive(jcell, "apn");
            if (cJSON_IsString(japn)) copy_str(out_cfg->net.cellular.apn, sizeof(out_cfg->net.cellular.apn), japn->valuestring);
            const cJSON *jau = cJSON_GetObjectItemCaseSensitive(jcell, "apn_user");
            if (cJSON_IsString(jau)) copy_str(out_cfg->net.cellular.apn_user, sizeof(out_cfg->net.cellular.apn_user), jau->valuestring);
            const cJSON *japw = cJSON_GetObjectItemCaseSensitive(jcell, "apn_pass");
            if (cJSON_IsString(japw)) copy_str(out_cfg->net.cellular.apn_pass, sizeof(out_cfg->net.cellular.apn_pass), japw->valuestring);
            const cJSON *jspin = cJSON_GetObjectItemCaseSensitive(jcell, "sim_pin");
            if (cJSON_IsString(jspin)) copy_str(out_cfg->net.cellular.sim_pin, sizeof(out_cfg->net.cellular.sim_pin), jspin->valuestring);
            const cJSON *jup = cJSON_GetObjectItemCaseSensitive(jcell, "uart_port");
            if (cJSON_IsNumber(jup)) out_cfg->net.cellular.uart_port = (uint8_t)jup->valueint;
            const cJSON *jbr = cJSON_GetObjectItemCaseSensitive(jcell, "baudrate");
            if (cJSON_IsNumber(jbr) && jbr->valueint >= 9600) out_cfg->net.cellular.baudrate = (uint32_t)jbr->valueint;
            const cJSON *jtx2 = cJSON_GetObjectItemCaseSensitive(jcell, "tx_pin");
            if (cJSON_IsNumber(jtx2)) out_cfg->net.cellular.tx_pin = (int8_t)jtx2->valueint;
            const cJSON *jrx2 = cJSON_GetObjectItemCaseSensitive(jcell, "rx_pin");
            if (cJSON_IsNumber(jrx2)) out_cfg->net.cellular.rx_pin = (int8_t)jrx2->valueint;
            const cJSON *jrts = cJSON_GetObjectItemCaseSensitive(jcell, "rts_pin");
            if (cJSON_IsNumber(jrts)) out_cfg->net.cellular.rts_pin = (int8_t)jrts->valueint;
            const cJSON *jcts = cJSON_GetObjectItemCaseSensitive(jcell, "cts_pin");
            if (cJSON_IsNumber(jcts)) out_cfg->net.cellular.cts_pin = (int8_t)jcts->valueint;
            const cJSON *jpk = cJSON_GetObjectItemCaseSensitive(jcell, "pwrkey_pin");
            if (cJSON_IsNumber(jpk)) out_cfg->net.cellular.pwrkey_pin = (int8_t)jpk->valueint;
            const cJSON *jrst = cJSON_GetObjectItemCaseSensitive(jcell, "reset_pin");
            if (cJSON_IsNumber(jrst)) out_cfg->net.cellular.reset_pin = (int8_t)jrst->valueint;
        }
    }

    /* optional: recipes (batch setpoint presets) */
    const cJSON *jrecipes = cJSON_GetObjectItemCaseSensitive(root, "recipes");
    if (cJSON_IsArray(jrecipes)) {
        const cJSON *jrec = NULL;
        cJSON_ArrayForEach(jrec, jrecipes) {
            if (out_cfg->recipe_count >= EDGEHMI_MAX_RECIPES) {
                set_err(err_buf, err_buf_len, "exceeded max recipes");
                cJSON_Delete(root);
                return EDGEHMI_ERR_FULL;
            }
            const cJSON *jrid = cJSON_GetObjectItemCaseSensitive(jrec, "id");
            const cJSON *jrname = cJSON_GetObjectItemCaseSensitive(jrec, "name");
            const cJSON *jitems = cJSON_GetObjectItemCaseSensitive(jrec, "items");
            if (!cJSON_IsString(jrid) || !cJSON_IsArray(jitems) || cJSON_GetArraySize(jitems) == 0) {
                set_err(err_buf, err_buf_len, "recipe requires id and non-empty items");
                cJSON_Delete(root);
                return EDGEHMI_ERR_SCHEMA;
            }
            edgehmi_recipe_t *rec = &out_cfg->recipes[out_cfg->recipe_count];
            memset(rec, 0, sizeof(*rec));
            copy_str(rec->id, sizeof(rec->id), jrid->valuestring);
            copy_str(rec->name, sizeof(rec->name), cJSON_IsString(jrname) ? jrname->valuestring : jrid->valuestring);

            const cJSON *jit = NULL;
            cJSON_ArrayForEach(jit, jitems) {
                if (rec->item_count >= EDGEHMI_MAX_RECIPE_ITEMS) {
                    set_err(err_buf, err_buf_len, "exceeded max recipe items");
                    cJSON_Delete(root);
                    return EDGEHMI_ERR_FULL;
                }
                const cJSON *jtag = cJSON_GetObjectItemCaseSensitive(jit, "tag");
                const cJSON *jval = cJSON_GetObjectItemCaseSensitive(jit, "value");
                if (!cJSON_IsString(jtag) || !cJSON_IsNumber(jval)) {
                    set_err(err_buf, err_buf_len, "recipe item requires tag and value");
                    cJSON_Delete(root);
                    return EDGEHMI_ERR_SCHEMA;
                }
                edgehmi_recipe_item_t *it = &rec->items[rec->item_count];
                copy_str(it->tag_name, sizeof(it->tag_name), jtag->valuestring);
                it->tag_idx = edgehmi_bus_find(bus, it->tag_name);
                if (it->tag_idx < 0) {
                    set_err(err_buf, err_buf_len, "recipe item references unknown tag");
                    cJSON_Delete(root);
                    return EDGEHMI_ERR_NOT_FOUND;
                }
                if ((bus->tags[it->tag_idx].access & TAG_ACCESS_W) == 0) {
                    set_err(err_buf, err_buf_len, "recipe item targets read-only tag");
                    cJSON_Delete(root);
                    return EDGEHMI_ERR_READONLY;
                }
                it->value = jval->valuedouble;
                rec->item_count++;
            }
            out_cfg->recipe_count++;
        }
    }

    /* optional: schedules (edge time-of-day or monotonic interval actions) */
    const cJSON *jscheds = cJSON_GetObjectItemCaseSensitive(root, "schedules");
    if (cJSON_IsArray(jscheds)) {
        const cJSON *jsc = NULL;
        cJSON_ArrayForEach(jsc, jscheds) {
            if (out_cfg->schedule_count >= EDGEHMI_MAX_SCHEDULES) {
                set_err(err_buf, err_buf_len, "exceeded max schedules");
                cJSON_Delete(root);
                return EDGEHMI_ERR_FULL;
            }
            const cJSON *jsid = cJSON_GetObjectItemCaseSensitive(jsc, "id");
            if (!cJSON_IsString(jsid) || jsid->valuestring[0] == '\0') {
                set_err(err_buf, err_buf_len, "schedule requires id");
                cJSON_Delete(root);
                return EDGEHMI_ERR_SCHEMA;
            }
            edgehmi_schedule_t *sc = &out_cfg->schedules[out_cfg->schedule_count];
            memset(sc, 0, sizeof(*sc));
            sc->enabled = true;
            sc->hour = -1;
            sc->days_mask = 0x7Fu;
            sc->target_tag_idx = -1;
            copy_str(sc->id, sizeof(sc->id), jsid->valuestring);

            const cJSON *jsen = cJSON_GetObjectItemCaseSensitive(jsc, "enabled");
            if (cJSON_IsBool(jsen)) sc->enabled = cJSON_IsTrue(jsen);
            const cJSON *jdmask = cJSON_GetObjectItemCaseSensitive(jsc, "days_mask");
            if (cJSON_IsNumber(jdmask) && jdmask->valueint > 0 && jdmask->valueint <= 127) {
                sc->days_mask = (uint8_t)jdmask->valueint;
            }
            const cJSON *jintv = cJSON_GetObjectItemCaseSensitive(jsc, "interval_s");
            const cJSON *jtime = cJSON_GetObjectItemCaseSensitive(jsc, "time");
            if (cJSON_IsNumber(jintv) && jintv->valueint > 0) {
                sc->interval_s = (uint32_t)jintv->valueint;
            } else if (cJSON_IsString(jtime) && strlen(jtime->valuestring) == 5 && jtime->valuestring[2] == ':') {
                int hh = atoi(jtime->valuestring);
                int mm = atoi(jtime->valuestring + 3);
                if (hh < 0 || hh > 23 || mm < 0 || mm > 59) {
                    set_err(err_buf, err_buf_len, "schedule time out of range (HH:MM)");
                    cJSON_Delete(root);
                    return EDGEHMI_ERR_SCHEMA;
                }
                sc->hour = (int8_t)hh;
                sc->minute = (int8_t)mm;
            } else {
                set_err(err_buf, err_buf_len, "schedule requires interval_s (>0) or time ('HH:MM')");
                cJSON_Delete(root);
                return EDGEHMI_ERR_SCHEMA;
            }

            const cJSON *jrecid = cJSON_GetObjectItemCaseSensitive(jsc, "recipe_id");
            const cJSON *jttag = cJSON_GetObjectItemCaseSensitive(jsc, "target_tag");
            const cJSON *jtval = cJSON_GetObjectItemCaseSensitive(jsc, "value");
            if (cJSON_IsString(jrecid) && jrecid->valuestring[0] != '\0') {
                sc->use_recipe = true;
                copy_str(sc->recipe_id, sizeof(sc->recipe_id), jrecid->valuestring);
                bool found = false;
                for (size_t r = 0; r < out_cfg->recipe_count; ++r) {
                    if (strcmp(out_cfg->recipes[r].id, sc->recipe_id) == 0) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    set_err(err_buf, err_buf_len, "schedule references unknown recipe_id");
                    cJSON_Delete(root);
                    return EDGEHMI_ERR_NOT_FOUND;
                }
            } else if (cJSON_IsString(jttag) && cJSON_IsNumber(jtval)) {
                copy_str(sc->target_tag, sizeof(sc->target_tag), jttag->valuestring);
                sc->target_tag_idx = edgehmi_bus_find(bus, sc->target_tag);
                if (sc->target_tag_idx < 0) {
                    set_err(err_buf, err_buf_len, "schedule references unknown target_tag");
                    cJSON_Delete(root);
                    return EDGEHMI_ERR_NOT_FOUND;
                }
                if ((bus->tags[sc->target_tag_idx].access & TAG_ACCESS_W) == 0) {
                    set_err(err_buf, err_buf_len, "schedule targets read-only tag");
                    cJSON_Delete(root);
                    return EDGEHMI_ERR_READONLY;
                }
                sc->write_value = jtval->valuedouble;
            } else {
                set_err(err_buf, err_buf_len, "schedule requires recipe_id or target_tag+value");
                cJSON_Delete(root);
                return EDGEHMI_ERR_SCHEMA;
            }
            out_cfg->schedule_count++;
        }
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
            w->vis_tag_idx = -1;
            w->type = wt;
            copy_str(w->id, sizeof(w->id), jwid->valuestring);
            w->x = (int16_t)jx->valueint;
            w->y = (int16_t)jy->valueint;
            w->w = (int16_t)jww->valueint;
            w->h = (int16_t)jwh->valueint;
            w->max_val = 100.0;
            w->step = 1.0;
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
            const cJSON *jstep = cJSON_GetObjectItemCaseSensitive(jw_item, "step");
            if (cJSON_IsNumber(jstep) && jstep->valuedouble > 0.0) w->step = jstep->valuedouble;
            const cJSON *jdec = cJSON_GetObjectItemCaseSensitive(jw_item, "decimals");
            if (cJSON_IsNumber(jdec) && jdec->valueint >= 0) w->decimals = (uint8_t)jdec->valueint;
            const cJSON *jcol = cJSON_GetObjectItemCaseSensitive(jw_item, "color");
            if (cJSON_IsString(jcol)) w->color_rgb = parse_hex_color(jcol->valuestring, 0x2DD4BFu);
            const cJSON *jsrc = cJSON_GetObjectItemCaseSensitive(jw_item, "src");
            if (cJSON_IsString(jsrc)) copy_str(w->src, sizeof(w->src), jsrc->valuestring);
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

            /* Optional dynamic threshold color_rules */
            const cJSON *jcr = cJSON_GetObjectItemCaseSensitive(jw_item, "color_rules");
            if (cJSON_IsArray(jcr)) {
                const cJSON *cr_it = NULL;
                cJSON_ArrayForEach(cr_it, jcr) {
                    if (w->color_rule_count >= EDGEHMI_MAX_COLOR_RULES) break;
                    const cJSON *cop = cJSON_GetObjectItemCaseSensitive(cr_it, "op");
                    const cJSON *cv  = cJSON_GetObjectItemCaseSensitive(cr_it, "value");
                    const cJSON *cc  = cJSON_GetObjectItemCaseSensitive(cr_it, "color");
                    edgehmi_rule_op_t op_enum = RULE_OP_GE;
                    if (cJSON_IsString(cop) && cJSON_IsNumber(cv) && cJSON_IsString(cc) &&
                        parse_rule_op_str(cop->valuestring, &op_enum)) {
                        edgehmi_color_rule_t *cr = &w->color_rules[w->color_rule_count++];
                        cr->op = op_enum;
                        cr->threshold = cv->valuedouble;
                        cr->color_rgb = parse_hex_color(cc->valuestring, w->color_rgb);
                    }
                }
            }

            /* Optional conditional visibility visible_when */
            const cJSON *jvis = cJSON_GetObjectItemCaseSensitive(jw_item, "visible_when");
            if (cJSON_IsObject(jvis)) {
                const cJSON *vt = cJSON_GetObjectItemCaseSensitive(jvis, "tag");
                const cJSON *vo = cJSON_GetObjectItemCaseSensitive(jvis, "op");
                const cJSON *vv = cJSON_GetObjectItemCaseSensitive(jvis, "value");
                edgehmi_rule_op_t vop = RULE_OP_EQ;
                if (cJSON_IsString(vt) && cJSON_IsString(vo) && cJSON_IsNumber(vv) &&
                    parse_rule_op_str(vo->valuestring, &vop)) {
                    int16_t vidx = edgehmi_bus_find(bus, vt->valuestring);
                    if (vidx < 0) {
                        set_err(err_buf, err_buf_len, "widget visible_when references unknown tag");
                        cJSON_Delete(root);
                        return EDGEHMI_ERR_NOT_FOUND;
                    }
                    w->has_vis_rule = true;
                    copy_str(w->vis_tag, sizeof(w->vis_tag), vt->valuestring);
                    w->vis_tag_idx = vidx;
                    w->vis_op = vop;
                    w->vis_threshold = vv->valuedouble;
                }
            }

            pg->widget_count++;
        }

        out_cfg->page_count++;
    }

    cJSON_Delete(root);
    return EDGEHMI_OK;
}
