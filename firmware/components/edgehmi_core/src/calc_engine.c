#include "calc_engine.h"

#include <math.h>

void edgehmi_calc_bind(edgehmi_calc_tag_t *calcs, size_t count, const edgehmi_bus_t *bus) {
    if (!calcs || !bus) {
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        calcs[i].tag_idx   = edgehmi_bus_find(bus, calcs[i].tag_name);
        calcs[i].src_a_idx = calcs[i].src_a[0] ? edgehmi_bus_find(bus, calcs[i].src_a) : -1;
        calcs[i].src_b_idx = calcs[i].src_b[0] ? edgehmi_bus_find(bus, calcs[i].src_b) : -1;
        calcs[i].last_ms   = 0;
    }
}

size_t edgehmi_calc_eval(edgehmi_calc_tag_t *calcs,
                         size_t count,
                         edgehmi_bus_t *bus,
                         uint64_t now_ms) {
    if (!calcs || !bus) {
        return 0;
    }
    size_t updated = 0;
    for (size_t i = 0; i < count; ++i) {
        edgehmi_calc_tag_t *c = &calcs[i];
        if (c->tag_idx < 0 || c->src_a_idx < 0) {
            continue;
        }
        double a = 0.0, b = 0.0;
        edgehmi_quality_t qa = TAG_QUAL_INIT, qb = TAG_QUAL_GOOD;
        if (edgehmi_bus_get(bus, c->src_a_idx, &a, &qa) != EDGEHMI_OK || qa != TAG_QUAL_GOOD) {
            continue;
        }
        if (c->op != CALC_OP_INTEGRATE) {
            if (c->src_b_idx < 0 ||
                edgehmi_bus_get(bus, c->src_b_idx, &b, &qb) != EDGEHMI_OK ||
                qb != TAG_QUAL_GOOD) {
                continue;
            }
        }

        double res = 0.0;
        double scale = (c->scale != 0.0) ? c->scale : 1.0;
        switch (c->op) {
            case CALC_OP_ADD:
                res = (a + b) * scale + c->offset;
                break;
            case CALC_OP_SUB:
                res = (a - b) * scale + c->offset;
                break;
            case CALC_OP_MUL:
                res = (a * b) * scale + c->offset;
                break;
            case CALC_OP_DIV:
                if (fabs(b) < 1e-9) {
                    continue; /* Guard against division by zero */
                }
                res = (a / b) * scale + c->offset;
                break;
            case CALC_OP_AVG:
                res = ((a + b) * 0.5) * scale + c->offset;
                break;
            case CALC_OP_INTEGRATE:
                if (c->last_ms > 0 && now_ms > c->last_ms) {
                    double dt_hours = (double)(now_ms - c->last_ms) / 3600000.0;
                    c->accum += a * dt_hours * scale;
                }
                c->last_ms = now_ms;
                res = c->accum + c->offset;
                break;
        }
        if (edgehmi_bus_set(bus, c->tag_idx, res, TAG_QUAL_GOOD, now_ms) == EDGEHMI_OK) {
            updated++;
        }
    }
    return updated;
}
