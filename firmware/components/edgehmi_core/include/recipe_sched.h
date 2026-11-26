#ifndef EDGEHMI_RECIPE_SCHED_H
#define EDGEHMI_RECIPE_SCHED_H

#include "data_bus.h"
#include "edgehmi_types.h"
#include "history_log.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Re-bind recipe and schedule tag indices onto an initialized data_bus */
void edgehmi_recipes_bind(edgehmi_config_t *cfg, const edgehmi_bus_t *bus);

/*
 * Atomically apply a named batch recipe by queuing writes for all items.
 * Optionally records each setpoint change in `hist` compliance audit trail.
 */
edgehmi_err_t edgehmi_recipe_apply(const edgehmi_config_t *cfg,
                                   const char *recipe_id,
                                   edgehmi_bus_t *bus,
                                   edgehmi_history_t *hist,
                                   const char *actor,
                                   uint64_t now_ms,
                                   size_t *out_applied);

/*
 * Evaluate all enabled edge schedules:
 *   - Interval schedules fire monotonically every `interval_s` seconds based on `now_ms`.
 *   - Time-of-day schedules (`hour >= 0`) fire once per matching calendar minute when `wall_time_valid` is true.
 *     `day_of_week`: 0=Sun .. 6=Sat; `epoch_day`: monotonic calendar day index (e.g. unix_sec / 86400).
 * Returns number of schedules fired in this tick.
 */
size_t edgehmi_schedules_eval(edgehmi_config_t *cfg,
                              edgehmi_bus_t *bus,
                              edgehmi_history_t *hist,
                              uint64_t now_ms,
                              bool wall_time_valid,
                              uint32_t epoch_day,
                              uint8_t day_of_week,
                              uint8_t hour,
                              uint8_t minute);

#ifdef __cplusplus
}
#endif

#endif /* EDGEHMI_RECIPE_SCHED_H */
