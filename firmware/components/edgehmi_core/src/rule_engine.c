#include "rule_engine.h"

#include <math.h>
#include <string.h>

static bool eval_single_cond(const edgehmi_rule_cond_t *c, bool rule_active, double val) {
    double h = c->hysteresis;
    switch (c->op) {
        case RULE_OP_GT:
            return rule_active ? (val > (c->threshold - h)) : (val > c->threshold);
        case RULE_OP_GE:
            return rule_active ? (val >= (c->threshold - h)) : (val >= c->threshold);
        case RULE_OP_LT:
            return rule_active ? (val < (c->threshold + h)) : (val < c->threshold);
        case RULE_OP_LE:
            return rule_active ? (val <= (c->threshold + h)) : (val <= c->threshold);
        case RULE_OP_EQ:
            return fabs(val - c->threshold) <= (h > 0.0 ? h : 1e-6);
        case RULE_OP_NE:
            return fabs(val - c->threshold) > (h > 0.0 ? h : 1e-6);
        default:
            return false;
    }
}

static bool eval_rule_conditions(const edgehmi_rule_t *r, edgehmi_bus_t *bus, bool *out_valid) {
    *out_valid = true;

    if (r->logic_mode == RULE_LOGIC_COMM_LOSS) {
        if (r->cond_count > 0) {
            for (size_t i = 0; i < r->cond_count; ++i) {
                double v = 0.0;
                edgehmi_quality_t q = TAG_QUAL_INIT;
                if (r->conds[i].tag_idx >= 0 &&
                    edgehmi_bus_get(bus, r->conds[i].tag_idx, &v, &q) == EDGEHMI_OK) {
                    if (q == TAG_QUAL_STALE || q == TAG_QUAL_BAD) {
                        return true;
                    }
                }
            }
            return false;
        }
        /* Monitor entire bus if no specific tag listed */
        for (size_t i = 0; i < bus->count; ++i) {
            double v = 0.0;
            edgehmi_quality_t q = TAG_QUAL_INIT;
            if (edgehmi_bus_get(bus, (int16_t)i, &v, &q) == EDGEHMI_OK) {
                if (q == TAG_QUAL_STALE || q == TAG_QUAL_BAD) {
                    return true;
                }
            }
        }
        return false;
    }

    if (r->cond_count == 0) {
        *out_valid = false;
        return false;
    }

    if (r->logic_mode == RULE_LOGIC_ALL) {
        for (size_t i = 0; i < r->cond_count; ++i) {
            const edgehmi_rule_cond_t *c = &r->conds[i];
            if (c->tag_idx < 0) {
                *out_valid = false;
                return false;
            }
            double val = 0.0;
            edgehmi_quality_t qual = TAG_QUAL_INIT;
            if (edgehmi_bus_get(bus, c->tag_idx, &val, &qual) != EDGEHMI_OK || qual != TAG_QUAL_GOOD) {
                *out_valid = false;
                return false;
            }
            if (!eval_single_cond(c, r->active, val)) {
                return false;
            }
        }
        return true;
    }

    /* RULE_LOGIC_ANY (OR) */
    size_t good_seen = 0;
    for (size_t i = 0; i < r->cond_count; ++i) {
        const edgehmi_rule_cond_t *c = &r->conds[i];
        if (c->tag_idx < 0) continue;
        double val = 0.0;
        edgehmi_quality_t qual = TAG_QUAL_INIT;
        if (edgehmi_bus_get(bus, c->tag_idx, &val, &qual) == EDGEHMI_OK && qual == TAG_QUAL_GOOD) {
            good_seen++;
            if (eval_single_cond(c, r->active, val)) {
                return true;
            }
        }
    }
    if (good_seen == 0) {
        *out_valid = false;
    }
    return false;
}

static void emit_alarm(edgehmi_rule_engine_t *eng,
                       const edgehmi_rule_t *r,
                       uint64_t now_ms,
                       bool active) {
    if (!eng->alarm_cb) {
        return;
    }
    edgehmi_alarm_event_t evt;
    memset(&evt, 0, sizeof(evt));
    edgehmi_copy_str(evt.rule_id, sizeof(evt.rule_id), r->id);
    evt.level = r->alarm_level;
    edgehmi_copy_str(evt.message, sizeof(evt.message), r->alarm_msg);
    evt.timestamp_ms = now_ms;
    evt.active = active;
    eng->alarm_cb(&evt, eng->alarm_ctx);
}

static void fire_rule_action(edgehmi_rule_engine_t *eng,
                             edgehmi_rule_t *r,
                             edgehmi_bus_t *bus,
                             uint64_t now_ms) {
    r->active = true;
    r->pending_trigger = false;
    r->trigger_count++;
    eng->total_triggers++;
    if (r->action == RULE_ACT_SET_TAG && r->target_tag_idx >= 0) {
        edgehmi_bus_request_write(bus, r->target_tag_idx, r->action_value, now_ms);
    } else if (r->action == RULE_ACT_ALARM) {
        emit_alarm(eng, r, now_ms, true);
    }
}

void edgehmi_rules_init(edgehmi_rule_engine_t *eng,
                        edgehmi_rule_t *rules,
                        size_t rule_count,
                        edgehmi_alarm_cb alarm_cb,
                        void *alarm_ctx) {
    if (!eng) {
        return;
    }
    memset(eng, 0, sizeof(*eng));
    eng->rules = rules;
    eng->rule_count = rule_count;
    eng->alarm_cb = alarm_cb;
    eng->alarm_ctx = alarm_ctx;
}

size_t edgehmi_rules_eval(edgehmi_rule_engine_t *eng,
                          edgehmi_bus_t *bus,
                          uint64_t now_ms) {
    if (!eng || !bus || !eng->rules) {
        return 0;
    }

    size_t triggered = 0;
    eng->total_evals++;

    for (size_t i = 0; i < eng->rule_count; ++i) {
        edgehmi_rule_t *r = &eng->rules[i];
        if (!r->enabled) {
            continue;
        }

        bool valid = false;
        bool cond_met = eval_rule_conditions(r, bus, &valid);
        if (!valid) {
            r->pending_trigger = false;
            continue;
        }

        if (cond_met) {
            if (!r->active) {
                if (r->debounce_ms == 0) {
                    fire_rule_action(eng, r, bus, now_ms);
                    triggered++;
                } else if (!r->pending_trigger) {
                    r->pending_trigger = true;
                    r->pending_since_ms = now_ms;
                } else if ((now_ms - r->pending_since_ms) >= (uint64_t)r->debounce_ms) {
                    fire_rule_action(eng, r, bus, now_ms);
                    triggered++;
                }
            } else if (r->enforce && r->action == RULE_ACT_SET_TAG && r->target_tag_idx >= 0) {
                /* Continuous safety interlock: re-assert safe value if manually overridden */
                double cur_val = 0.0;
                edgehmi_quality_t cur_q = TAG_QUAL_INIT;
                if (edgehmi_bus_get(bus, r->target_tag_idx, &cur_val, &cur_q) == EDGEHMI_OK &&
                    fabs(cur_val - r->action_value) > 1e-6) {
                    edgehmi_bus_request_write(bus, r->target_tag_idx, r->action_value, now_ms);
                    r->trigger_count++;
                    eng->total_triggers++;
                    triggered++;
                }
            }
        } else {
            r->pending_trigger = false;
            if (r->active) {
                r->active = false;
                if (r->action == RULE_ACT_ALARM) {
                    emit_alarm(eng, r, now_ms, false);
                }
            }
        }
    }

    return triggered;
}
