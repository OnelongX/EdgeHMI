#ifndef EDGEHMI_HISTORY_LOG_H
#define EDGEHMI_HISTORY_LOG_H

#include "data_bus.h"
#include "edgehmi_types.h"
#include "rule_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t timestamp_ms;
    float values[EDGEHMI_MAX_HISTORY_TAGS];
} edgehmi_hist_sample_t;

typedef struct {
    uint32_t id;
    uint64_t timestamp_ms;
    edgehmi_alarm_level_t level;
    bool active;
    bool acknowledged;
    char rule_id[EDGEHMI_MAX_NAME_LEN];
    char message[EDGEHMI_MAX_TEXT_LEN];
} edgehmi_alarm_record_t;

typedef struct {
    char tag_names[EDGEHMI_MAX_HISTORY_TAGS][EDGEHMI_MAX_NAME_LEN];
    int16_t tag_indices[EDGEHMI_MAX_HISTORY_TAGS];
    size_t tag_count;

    edgehmi_hist_sample_t samples[EDGEHMI_MAX_HISTORY_SAMPLES];
    size_t head;
    size_t count;

    edgehmi_alarm_record_t alarms[EDGEHMI_MAX_ALARMS];
    size_t alarm_head;
    size_t alarm_count;
    uint32_t next_alarm_id;
} edgehmi_history_t;

/* Initialize historian and bind up to EDGEHMI_MAX_HISTORY_TAGS readable bus tags */
void edgehmi_history_init(edgehmi_history_t *hist, const edgehmi_bus_t *bus);

/* Append one timestamped telemetry snapshot from bus */
void edgehmi_history_record_sample(edgehmi_history_t *hist, edgehmi_bus_t *bus, uint64_t now_ms);

/* Record an alarm event into the circular audit buffer */
uint32_t edgehmi_history_record_alarm(edgehmi_history_t *hist, const edgehmi_alarm_event_t *evt);

/* Acknowledge an alarm by id (or alarm_id == 0 to acknowledge all) */
size_t edgehmi_history_ack_alarm(edgehmi_history_t *hist, uint32_t alarm_id);

/* Export recorded time-series history as RFC 4180 CSV into caller buffer */
size_t edgehmi_history_export_csv(const edgehmi_history_t *hist, char *out_buf, size_t out_cap);

/* Export alarm audit list as JSON array into caller buffer */
size_t edgehmi_history_export_alarms_json(const edgehmi_history_t *hist, char *out_buf, size_t out_cap);

/* Persist / restore history & alarm audit buffer to LittleFS/SD file across power cycles */
edgehmi_err_t edgehmi_history_save_file(const edgehmi_history_t *hist, const char *path);
edgehmi_err_t edgehmi_history_load_file(edgehmi_history_t *hist, const char *path);

#ifdef __cplusplus
}
#endif

#endif /* EDGEHMI_HISTORY_LOG_H */
