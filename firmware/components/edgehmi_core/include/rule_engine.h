#ifndef EDGEHMI_RULE_ENGINE_H
#define EDGEHMI_RULE_ENGINE_H

#include "data_bus.h"
#include "edgehmi_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char rule_id[EDGEHMI_MAX_NAME_LEN];
    edgehmi_alarm_level_t level;
    char message[EDGEHMI_MAX_TEXT_LEN];
    uint64_t timestamp_ms;
    bool active;
} edgehmi_alarm_event_t;

typedef void (*edgehmi_alarm_cb)(const edgehmi_alarm_event_t *evt, void *user_ctx);

typedef struct {
    edgehmi_rule_t *rules;
    size_t rule_count;
    edgehmi_alarm_cb alarm_cb;
    void *alarm_ctx;
    uint32_t total_evals;
    uint32_t total_triggers;
} edgehmi_rule_engine_t;

void edgehmi_rules_init(edgehmi_rule_engine_t *eng,
                        edgehmi_rule_t *rules,
                        size_t rule_count,
                        edgehmi_alarm_cb alarm_cb,
                        void *alarm_ctx);

/* Deterministic evaluation tick: evaluates all rules with hysteresis & debounce */
size_t edgehmi_rules_eval(edgehmi_rule_engine_t *eng,
                          edgehmi_bus_t *bus,
                          uint64_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* EDGEHMI_RULE_ENGINE_H */
