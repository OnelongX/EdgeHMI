#include "history_log.h"

#include <stdio.h>
#include <string.h>

void edgehmi_history_init(edgehmi_history_t *hist, const edgehmi_bus_t *bus) {
    if (!hist) {
        return;
    }
    memset(hist, 0, sizeof(*hist));
    hist->next_alarm_id = 1;
    if (!bus) {
        return;
    }
    for (size_t i = 0; i < bus->count && hist->tag_count < EDGEHMI_MAX_HISTORY_TAGS; ++i) {
        if ((bus->tags[i].access & TAG_ACCESS_R) != 0) {
            strncpy(hist->tag_names[hist->tag_count], bus->tags[i].name, EDGEHMI_MAX_NAME_LEN - 1);
            hist->tag_indices[hist->tag_count] = (int16_t)i;
            hist->tag_count++;
        }
    }
}

void edgehmi_history_record_sample(edgehmi_history_t *hist, edgehmi_bus_t *bus, uint64_t now_ms) {
    if (!hist || !bus || hist->tag_count == 0) {
        return;
    }
    edgehmi_hist_sample_t *slot = &hist->samples[hist->head];
    slot->timestamp_ms = now_ms;
    for (size_t i = 0; i < hist->tag_count; ++i) {
        double v = 0.0;
        edgehmi_bus_get(bus, hist->tag_indices[i], &v, NULL);
        slot->values[i] = (float)v;
    }
    hist->head = (hist->head + 1u) % EDGEHMI_MAX_HISTORY_SAMPLES;
    if (hist->count < EDGEHMI_MAX_HISTORY_SAMPLES) {
        hist->count++;
    }
}

uint32_t edgehmi_history_record_alarm(edgehmi_history_t *hist, const edgehmi_alarm_event_t *evt) {
    if (!hist || !evt) {
        return 0;
    }
    edgehmi_alarm_record_t *rec = &hist->alarms[hist->alarm_head];
    memset(rec, 0, sizeof(*rec));
    rec->id = hist->next_alarm_id++;
    rec->timestamp_ms = evt->timestamp_ms;
    rec->level = evt->level;
    rec->active = evt->active;
    rec->acknowledged = false;
    strncpy(rec->rule_id, evt->rule_id, sizeof(rec->rule_id) - 1);
    strncpy(rec->message, evt->message, sizeof(rec->message) - 1);

    hist->alarm_head = (hist->alarm_head + 1u) % EDGEHMI_MAX_ALARMS;
    if (hist->alarm_count < EDGEHMI_MAX_ALARMS) {
        hist->alarm_count++;
    }
    return rec->id;
}

size_t edgehmi_history_ack_alarm(edgehmi_history_t *hist, uint32_t alarm_id) {
    if (!hist) {
        return 0;
    }
    size_t acked = 0;
    for (size_t i = 0; i < hist->alarm_count; ++i) {
        if ((alarm_id == 0 || hist->alarms[i].id == alarm_id) && !hist->alarms[i].acknowledged) {
            hist->alarms[i].acknowledged = true;
            acked++;
        }
    }
    return acked;
}

size_t edgehmi_history_export_csv(const edgehmi_history_t *hist, char *out_buf, size_t out_cap) {
    if (!hist || !out_buf || out_cap < 32) {
        return 0;
    }
    size_t pos = (size_t)snprintf(out_buf, out_cap, "timestamp_ms");
    for (size_t c = 0; c < hist->tag_count && pos < out_cap - 4; ++c) {
        pos += (size_t)snprintf(out_buf + pos, out_cap - pos, ",%s", hist->tag_names[c]);
    }
    if (pos < out_cap - 2) {
        out_buf[pos++] = '\n';
        out_buf[pos] = '\0';
    }

    size_t start = (hist->count == EDGEHMI_MAX_HISTORY_SAMPLES) ? hist->head : 0;
    for (size_t r = 0; r < hist->count && pos < out_cap - 64; ++r) {
        size_t idx = (start + r) % EDGEHMI_MAX_HISTORY_SAMPLES;
        const edgehmi_hist_sample_t *s = &hist->samples[idx];
        pos += (size_t)snprintf(out_buf + pos, out_cap - pos, "%llu",
                                (unsigned long long)s->timestamp_ms);
        for (size_t c = 0; c < hist->tag_count && pos < out_cap - 16; ++c) {
            pos += (size_t)snprintf(out_buf + pos, out_cap - pos, ",%.2f", (double)s->values[c]);
        }
        if (pos < out_cap - 2) {
            out_buf[pos++] = '\n';
            out_buf[pos] = '\0';
        }
    }
    return pos;
}

size_t edgehmi_history_export_alarms_json(const edgehmi_history_t *hist, char *out_buf, size_t out_cap) {
    if (!hist || !out_buf || out_cap < 32) {
        return 0;
    }
    size_t pos = (size_t)snprintf(out_buf, out_cap, "{\"alarms\":[");
    /* Emit newest-first */
    for (size_t n = 0; n < hist->alarm_count && pos < out_cap - 196; ++n) {
        size_t idx = (hist->alarm_head + EDGEHMI_MAX_ALARMS - 1u - n) % EDGEHMI_MAX_ALARMS;
        const edgehmi_alarm_record_t *a = &hist->alarms[idx];
        char esc_rule[64];
        char esc_msg[192];
        edgehmi_json_escape(a->rule_id, esc_rule, sizeof(esc_rule));
        edgehmi_json_escape(a->message, esc_msg, sizeof(esc_msg));
        const char *lvl = (a->level == ALARM_CRITICAL) ? "critical"
                          : (a->level == ALARM_WARN)   ? "warn"
                                                       : "info";
        pos += (size_t)snprintf(
            out_buf + pos, out_cap - pos,
            "%s{\"id\":%lu,\"ts_ms\":%llu,\"rule\":\"%s\",\"level\":\"%s\","
            "\"active\":%s,\"ack\":%s,\"message\":\"%s\"}",
            (n > 0) ? "," : "",
            (unsigned long)a->id,
            (unsigned long long)a->timestamp_ms,
            esc_rule, lvl,
            a->active ? "true" : "false",
            a->acknowledged ? "true" : "false",
            esc_msg);
    }
    pos += (size_t)snprintf(out_buf + pos, out_cap - pos, "]}");
    return pos;
}
