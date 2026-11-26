#include "history_log.h"

#include <stdio.h>
#include <string.h>

void edgehmi_history_init(edgehmi_history_t *hist, const edgehmi_bus_t *bus) {
    if (!hist) {
        return;
    }
    memset(hist, 0, sizeof(*hist));
    hist->next_alarm_id = 1;
    hist->next_audit_id = 1;
    if (!bus) {
        return;
    }
    for (size_t i = 0; i < bus->count && hist->tag_count < EDGEHMI_MAX_HISTORY_TAGS; ++i) {
        if ((bus->tags[i].access & TAG_ACCESS_R) != 0) {
            edgehmi_copy_str(hist->tag_names[hist->tag_count], EDGEHMI_MAX_NAME_LEN, bus->tags[i].name);
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
    edgehmi_copy_str(rec->rule_id, sizeof(rec->rule_id), evt->rule_id);
    edgehmi_copy_str(rec->message, sizeof(rec->message), evt->message);

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

static const char *audit_src_str(edgehmi_audit_src_t src) {
    switch (src) {
        case AUDIT_SRC_UI:        return "ui";
        case AUDIT_SRC_HTTP:      return "http";
        case AUDIT_SRC_MQTT:      return "mqtt";
        case AUDIT_SRC_MODBUS_GW: return "modbus_gw";
        case AUDIT_SRC_RECIPE:    return "recipe";
        case AUDIT_SRC_SCHEDULE:  return "schedule";
        case AUDIT_SRC_RULE:      return "rule";
        default:                  return "unknown";
    }
}

uint32_t edgehmi_history_record_audit(edgehmi_history_t *hist,
                                      edgehmi_audit_src_t source,
                                      const char *actor,
                                      const char *tag_name,
                                      double old_val,
                                      double new_val,
                                      uint64_t now_ms) {
    if (!hist || !tag_name) {
        return 0;
    }
    edgehmi_audit_record_t *rec = &hist->audits[hist->audit_head];
    memset(rec, 0, sizeof(*rec));
    rec->id = hist->next_audit_id++;
    rec->timestamp_ms = now_ms;
    rec->source = source;
    edgehmi_copy_str(rec->actor, sizeof(rec->actor), actor ? actor : "operator");
    edgehmi_copy_str(rec->tag_name, sizeof(rec->tag_name), tag_name);
    rec->old_val = (float)old_val;
    rec->new_val = (float)new_val;

    hist->audit_head = (hist->audit_head + 1u) % EDGEHMI_MAX_AUDIT_LOGS;
    if (hist->audit_count < EDGEHMI_MAX_AUDIT_LOGS) {
        hist->audit_count++;
    }
    return rec->id;
}

size_t edgehmi_history_export_audit_json(const edgehmi_history_t *hist, char *out_buf, size_t out_cap) {
    if (!hist || !out_buf || out_cap < 32) {
        return 0;
    }
    size_t pos = (size_t)snprintf(out_buf, out_cap, "{\"audits\":[");
    for (size_t n = 0; n < hist->audit_count && pos < out_cap - 196; ++n) {
        size_t idx = (hist->audit_head + EDGEHMI_MAX_AUDIT_LOGS - 1u - n) % EDGEHMI_MAX_AUDIT_LOGS;
        const edgehmi_audit_record_t *a = &hist->audits[idx];
        char esc_actor[48];
        char esc_tag[64];
        edgehmi_json_escape(a->actor, esc_actor, sizeof(esc_actor));
        edgehmi_json_escape(a->tag_name, esc_tag, sizeof(esc_tag));
        pos += (size_t)snprintf(
            out_buf + pos, out_cap - pos,
            "%s{\"id\":%lu,\"ts_ms\":%llu,\"source\":\"%s\",\"actor\":\"%s\","
            "\"tag\":\"%s\",\"old\":%.4f,\"new\":%.4f}",
            (n > 0) ? "," : "",
            (unsigned long)a->id,
            (unsigned long long)a->timestamp_ms,
            audit_src_str(a->source),
            esc_actor,
            esc_tag,
            (double)a->old_val,
            (double)a->new_val);
    }
    pos += (size_t)snprintf(out_buf + pos, out_cap - pos, "]}");
    return pos;
}

#define EDGEHMI_HIST_MAGIC 0x45484832u /* "EHH2" */

typedef struct {
    uint32_t magic;
    uint32_t struct_size;
    uint32_t tag_count;
} hist_file_hdr_t;

edgehmi_err_t edgehmi_history_save_file(const edgehmi_history_t *hist, const char *path) {
    if (!hist || !path) {
        return EDGEHMI_ERR_INVALID_ARG;
    }
    FILE *f = fopen(path, "wb");
    if (!f) {
        return EDGEHMI_ERR_IO;
    }
    hist_file_hdr_t hdr = {
        .magic = EDGEHMI_HIST_MAGIC,
        .struct_size = (uint32_t)sizeof(*hist),
        .tag_count = (uint32_t)hist->tag_count,
    };
    bool ok = (fwrite(&hdr, sizeof(hdr), 1, f) == 1) &&
              (fwrite(hist, sizeof(*hist), 1, f) == 1);
    fclose(f);
    return ok ? EDGEHMI_OK : EDGEHMI_ERR_IO;
}

edgehmi_err_t edgehmi_history_load_file(edgehmi_history_t *hist, const char *path) {
    if (!hist || !path) {
        return EDGEHMI_ERR_INVALID_ARG;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        return EDGEHMI_ERR_NOT_FOUND;
    }
    hist_file_hdr_t hdr;
    if (fread(&hdr, sizeof(hdr), 1, f) != 1 ||
        hdr.magic != EDGEHMI_HIST_MAGIC ||
        hdr.struct_size != (uint32_t)sizeof(*hist)) {
        fclose(f);
        return EDGEHMI_ERR_PARSE;
    }
    edgehmi_history_t tmp;
    if (fread(&tmp, sizeof(tmp), 1, f) != 1) {
        fclose(f);
        return EDGEHMI_ERR_IO;
    }
    fclose(f);
    /* Restore samples and alarms only if tag layout matches active bus */
    if (tmp.tag_count == hist->tag_count &&
        memcmp(tmp.tag_names, hist->tag_names, sizeof(tmp.tag_names)) == 0) {
        memcpy(hist->samples, tmp.samples, sizeof(hist->samples));
        hist->head = tmp.head % EDGEHMI_MAX_HISTORY_SAMPLES;
        hist->count = tmp.count <= EDGEHMI_MAX_HISTORY_SAMPLES ? tmp.count : EDGEHMI_MAX_HISTORY_SAMPLES;
    }
    memcpy(hist->alarms, tmp.alarms, sizeof(hist->alarms));
    hist->alarm_head = tmp.alarm_head % EDGEHMI_MAX_ALARMS;
    hist->alarm_count = tmp.alarm_count <= EDGEHMI_MAX_ALARMS ? tmp.alarm_count : EDGEHMI_MAX_ALARMS;
    hist->next_alarm_id = tmp.next_alarm_id > 0 ? tmp.next_alarm_id : 1;
    memcpy(hist->audits, tmp.audits, sizeof(hist->audits));
    hist->audit_head = tmp.audit_head % EDGEHMI_MAX_AUDIT_LOGS;
    hist->audit_count = tmp.audit_count <= EDGEHMI_MAX_AUDIT_LOGS ? tmp.audit_count : EDGEHMI_MAX_AUDIT_LOGS;
    hist->next_audit_id = tmp.next_audit_id > 0 ? tmp.next_audit_id : 1;
    return EDGEHMI_OK;
}
