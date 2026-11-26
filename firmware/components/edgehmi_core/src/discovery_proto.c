#include "discovery_proto.h"

#include "cJSON.h"
#include "data_bus.h"

#include <stdio.h>
#include <string.h>

bool edgehmi_discovery_is_probe(const char *msg, size_t len) {
    if (!msg || len == 0 || len > 512) {
        return false;
    }
    char tmp[513];
    memcpy(tmp, msg, len);
    tmp[len] = '\0';

    cJSON *root = cJSON_Parse(tmp);
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return false;
    }
    const cJSON *cmd = cJSON_GetObjectItemCaseSensitive(root, "cmd");
    bool ok = cJSON_IsString(cmd) && strcmp(cmd->valuestring, "discover") == 0;
    cJSON_Delete(root);
    return ok;
}

size_t edgehmi_discovery_build_announce(const edgehmi_device_info_t *info,
                                        char *out_buf,
                                        size_t out_cap) {
    if (!info || !out_buf || out_cap < 64) {
        return 0;
    }
    char esc_id[64];
    char esc_name[96];
    char esc_fw[32];
    char esc_cfg[32];
    edgehmi_json_escape(info->device_id, esc_id, sizeof(esc_id));
    edgehmi_json_escape(info->name, esc_name, sizeof(esc_name));
    edgehmi_json_escape(info->fw_version, esc_fw, sizeof(esc_fw));
    edgehmi_json_escape(info->cfg_version, esc_cfg, sizeof(esc_cfg));

    int n = snprintf(
        out_buf, out_cap,
        "{\"type\":\"edgehmi_device\",\"id\":\"%s\",\"name\":\"%s\","
        "\"fw_ver\":\"%s\",\"cfg_ver\":\"%s\",\"http_port\":%u,"
        "\"width\":%u,\"height\":%u,\"comm_loss\":%s,\"uptime_s\":%lu}",
        esc_id,
        esc_name,
        esc_fw,
        esc_cfg,
        (unsigned)info->http_port,
        (unsigned)info->width,
        (unsigned)info->height,
        info->comm_loss ? "true" : "false",
        (unsigned long)info->uptime_s);
    if (n < 0 || (size_t)n >= out_cap) {
        return 0;
    }
    return (size_t)n;
}
