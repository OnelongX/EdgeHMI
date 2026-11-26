#include "recipe_sched.h"

#include <string.h>

void edgehmi_recipes_bind(edgehmi_config_t *cfg, const edgehmi_bus_t *bus) {
    if (!cfg || !bus) {
        return;
    }
    for (size_t r = 0; r < cfg->recipe_count; ++r) {
        edgehmi_recipe_t *rec = &cfg->recipes[r];
        for (size_t i = 0; i < rec->item_count; ++i) {
            rec->items[i].tag_idx = edgehmi_bus_find(bus, rec->items[i].tag_name);
        }
    }
    for (size_t s = 0; s < cfg->schedule_count; ++s) {
        edgehmi_schedule_t *sc = &cfg->schedules[s];
        sc->last_fire_key = 0;
        if (!sc->use_recipe && sc->target_tag[0] != '\0') {
            sc->target_tag_idx = edgehmi_bus_find(bus, sc->target_tag);
        } else {
            sc->target_tag_idx = -1;
        }
    }
}

edgehmi_err_t edgehmi_recipe_apply(const edgehmi_config_t *cfg,
                                   const char *recipe_id,
                                   edgehmi_bus_t *bus,
                                   edgehmi_history_t *hist,
                                   const char *actor,
                                   uint64_t now_ms,
                                   size_t *out_applied) {
    if (out_applied) *out_applied = 0;
    if (!cfg || !recipe_id || !bus) {
        return EDGEHMI_ERR_INVALID_ARG;
    }

    const edgehmi_recipe_t *target = NULL;
    for (size_t r = 0; r < cfg->recipe_count; ++r) {
        if (strcmp(cfg->recipes[r].id, recipe_id) == 0) {
            target = &cfg->recipes[r];
            break;
        }
    }
    if (!target) {
        return EDGEHMI_ERR_NOT_FOUND;
    }

    /* Pre-check all items so we never half-apply a broken recipe */
    for (size_t i = 0; i < target->item_count; ++i) {
        int16_t idx = target->items[i].tag_idx;
        if (idx < 0 || (size_t)idx >= bus->count) {
            return EDGEHMI_ERR_NOT_FOUND;
        }
        if ((bus->tags[idx].access & TAG_ACCESS_W) == 0) {
            return EDGEHMI_ERR_READONLY;
        }
    }

    size_t applied = 0;
    for (size_t i = 0; i < target->item_count; ++i) {
        int16_t idx = target->items[i].tag_idx;
        double old_v = 0.0;
        edgehmi_bus_get(bus, idx, &old_v, NULL);
        double new_v = target->items[i].value;
        if (edgehmi_bus_request_write(bus, idx, new_v, now_ms) == EDGEHMI_OK) {
            if (hist) {
                edgehmi_history_record_audit(
                    hist, AUDIT_SRC_RECIPE,
                    actor ? actor : target->id,
                    target->items[i].tag_name, old_v, new_v, now_ms);
            }
            applied++;
        }
    }
    if (out_applied) *out_applied = applied;
    return EDGEHMI_OK;
}

size_t edgehmi_schedules_eval(edgehmi_config_t *cfg,
                              edgehmi_bus_t *bus,
                              edgehmi_history_t *hist,
                              uint64_t now_ms,
                              bool wall_time_valid,
                              uint32_t epoch_day,
                              uint8_t day_of_week,
                              uint8_t hour,
                              uint8_t minute) {
    if (!cfg || !bus) {
        return 0;
    }
    size_t fired = 0;
    for (size_t i = 0; i < cfg->schedule_count; ++i) {
        edgehmi_schedule_t *sc = &cfg->schedules[i];
        if (!sc->enabled) {
            continue;
        }
        bool should_fire = false;
        uint64_t fire_key = 0;

        if (sc->interval_s > 0) {
            uint64_t bucket = (now_ms / 1000ULL) / (uint64_t)sc->interval_s;
            if (bucket > 0 && bucket != sc->last_fire_key) {
                should_fire = true;
                fire_key = bucket;
            }
        } else if (sc->hour >= 0 && wall_time_valid && day_of_week <= 6) {
            uint8_t mask = sc->days_mask ? sc->days_mask : 0x7Fu;
            if ((mask & (uint8_t)(1u << day_of_week)) != 0 &&
                (uint8_t)sc->hour == hour &&
                (uint8_t)sc->minute == minute) {
                fire_key = ((uint64_t)epoch_day + 1ULL) * 1440ULL + (uint64_t)hour * 60ULL + (uint64_t)minute;
                if (fire_key != sc->last_fire_key) {
                    should_fire = true;
                }
            }
        }

        if (!should_fire) {
            continue;
        }
        sc->last_fire_key = fire_key;

        if (sc->use_recipe) {
            if (edgehmi_recipe_apply(cfg, sc->recipe_id, bus, hist, sc->id, now_ms, NULL) == EDGEHMI_OK) {
                fired++;
            }
        } else if (sc->target_tag_idx >= 0) {
            double old_v = 0.0;
            edgehmi_bus_get(bus, sc->target_tag_idx, &old_v, NULL);
            if (edgehmi_bus_request_write(bus, sc->target_tag_idx, sc->write_value, now_ms) == EDGEHMI_OK) {
                if (hist) {
                    edgehmi_history_record_audit(
                        hist, AUDIT_SRC_SCHEDULE, sc->id,
                        sc->target_tag, old_v, sc->write_value, now_ms);
                }
                fired++;
            }
        }
    }
    return fired;
}
