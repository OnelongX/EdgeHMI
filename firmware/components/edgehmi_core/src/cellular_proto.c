#include "cellular_proto.h"

#include "data_bus.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool edgehmi_cell_parse_csq(const char *resp, int8_t *out_dbm, uint8_t *out_bars) {
    if (!resp) return false;
    const char *p = strstr(resp, "+CSQ:");
    if (!p) return false;
    p += 5;
    while (*p == ' ') p++;
    char *endp = NULL;
    long rssi = strtol(p, &endp, 10);
    if (endp == p || rssi < 0 || rssi > 99) return false;

    if (rssi == 99) {
        if (out_dbm) *out_dbm = 0;
        if (out_bars) *out_bars = 0;
        return true;
    }
    int dbm = -113 + (int)rssi * 2;
    uint8_t bars = 0;
    if (dbm >= -75)      bars = 4;
    else if (dbm >= -85) bars = 3;
    else if (dbm >= -95) bars = 2;
    else if (dbm >= -105) bars = 1;

    if (out_dbm) *out_dbm = (int8_t)dbm;
    if (out_bars) *out_bars = bars;
    return true;
}

bool edgehmi_cell_parse_reg(const char *resp, bool *out_registered) {
    if (!resp || !out_registered) return false;
    /* Supports 2G/3G (+CREG), 4G LTE (+CEREG), and 5G NR (+C5GREG) */
    const char *p = strstr(resp, "+C5GREG:");
    if (!p) p = strstr(resp, "+CEREG:");
    if (!p) p = strstr(resp, "+CREG:");
    if (!p) return false;

    const char *comma = strchr(p, ',');
    if (!comma) return false;
    long stat = strtol(comma + 1, NULL, 10);
    /* 1 = registered home network, 5 = registered roaming */
    *out_registered = (stat == 1 || stat == 5);
    return true;
}

bool edgehmi_cell_parse_nwinfo(const char *resp,
                               edgehmi_cell_rat_t *out_rat,
                               char *out_oper,
                               size_t oper_cap) {
    if (!resp) return false;
    /* Check Quectel/Fibocom +QNWINFO, SIMCom +CPSI, or 3GPP +COPS */
    const char *p = strstr(resp, "+QNWINFO:");
    if (!p) p = strstr(resp, "+CPSI:");
    if (!p) p = strstr(resp, "+COPS:");
    if (!p) return false;

    edgehmi_cell_rat_t rat = CELL_RAT_NONE;
    if (strstr(p, "NR5G-SA") || strstr(p, "5G SA") || strstr(p, "5GSA")) {
        rat = CELL_RAT_5G_SA;
    } else if (strstr(p, "NR5G-NSA") || strstr(p, "ENDC") || strstr(p, "5G NSA")) {
        rat = CELL_RAT_5G_NSA;
    } else if (strstr(p, "REDCAP") || strstr(p, "RedCap")) {
        rat = CELL_RAT_5G_REDCAP;
    } else if (strstr(p, "NR5G") || strstr(p, "5G")) {
        rat = CELL_RAT_5G_SA;
    } else if (strstr(p, "CAT1") || strstr(p, "Cat.1") || strstr(p, "E-UTRAN")) {
        rat = CELL_RAT_4G_CAT1;
    } else if (strstr(p, "LTE") || strstr(p, "4G")) {
        rat = CELL_RAT_4G_LTE;
    } else if (strstr(p, "WCDMA") || strstr(p, "GSM")) {
        rat = CELL_RAT_2G_3G;
    }

    if (out_rat) *out_rat = rat;

    if (out_oper && oper_cap > 1) {
        out_oper[0] = '\0';
        const char *q1 = strchr(p, '"');
        if (q1) {
            const char *q2 = strchr(q1 + 1, '"');
            /* For +QNWINFO: "NR5G-SA","46000",... grab second quoted field if present */
            if (strncmp(p, "+QNWINFO:", 9) == 0 && q2) {
                const char *q3 = strchr(q2 + 1, '"');
                const char *q4 = q3 ? strchr(q3 + 1, '"') : NULL;
                if (q3 && q4 && q4 > q3 + 1) {
                    q1 = q3;
                    q2 = q4;
                }
            }
            if (q2 && q2 > q1 + 1) {
                size_t n = (size_t)(q2 - (q1 + 1));
                if (n >= oper_cap) n = oper_cap - 1;
                memcpy(out_oper, q1 + 1, n);
                out_oper[n] = '\0';
            }
        }
    }
    return rat != CELL_RAT_NONE || (out_oper && out_oper[0] != '\0');
}

bool edgehmi_cell_parse_iccid(const char *resp, char *out_iccid, size_t iccid_cap) {
    if (!resp || !out_iccid || iccid_cap < 16) return false;
    const char *p = strstr(resp, "CCID:");
    if (!p) p = resp;
    else p += 5;

    while (*p && !isdigit((unsigned char)*p)) p++;
    size_t n = 0;
    while (p[n] && (isdigit((unsigned char)p[n]) || (p[n] >= 'A' && p[n] <= 'F') || (p[n] >= 'a' && p[n] <= 'f'))) {
        if (n + 1 >= iccid_cap) break;
        out_iccid[n] = p[n];
        n++;
    }
    out_iccid[n] = '\0';
    return n >= 18 && n <= 22;
}

const char *edgehmi_cell_rat_str(edgehmi_cell_rat_t rat) {
    switch (rat) {
        case CELL_RAT_2G_3G:     return "2G/3G";
        case CELL_RAT_4G_CAT1:   return "4G Cat.1";
        case CELL_RAT_4G_LTE:    return "4G LTE";
        case CELL_RAT_5G_REDCAP: return "5G RedCap";
        case CELL_RAT_5G_NSA:    return "5G NSA";
        case CELL_RAT_5G_SA:     return "5G SA";
        default:                 return "OFFLINE";
    }
}

size_t edgehmi_cell_build_apn_cmd(const char *apn, char *out_buf, size_t out_cap) {
    if (!out_buf || out_cap < 24) return 0;
    const char *use_apn = (apn && apn[0]) ? apn : "cmnet";
    int n = snprintf(out_buf, out_cap, "AT+CGDCONT=1,\"IP\",\"%s\"\r\n", use_apn);
    return (n > 0 && (size_t)n < out_cap) ? (size_t)n : 0;
}

size_t edgehmi_cell_build_rat_cmd(edgehmi_cell_gen_t gen, char *out_buf, size_t out_cap) {
    if (!out_buf || out_cap < 24) return 0;
    const char *mode = "AUTO";
    switch (gen) {
        case CELL_GEN_4G:        mode = "LTE"; break;
        case CELL_GEN_5G:        mode = "NR5G"; break;
        case CELL_GEN_5G_REDCAP: mode = "NR5G:LTE"; break;
        default:                 mode = "AUTO"; break;
    }
    int n = snprintf(out_buf, out_cap, "AT+QNWPREFCFG=\"mode_pref\",%s\r\n", mode);
    return (n > 0 && (size_t)n < out_cap) ? (size_t)n : 0;
}

void edgehmi_wan_failover_init(edgehmi_wan_failover_t *fm, edgehmi_net_policy_t policy) {
    if (!fm) return;
    memset(fm, 0, sizeof(*fm));
    fm->policy = policy;
    fm->active_link = WAN_LINK_OFFLINE;
    fm->failover_threshold = 3;
    fm->failback_threshold = 5;
}

edgehmi_wan_link_t edgehmi_wan_failover_tick(edgehmi_wan_failover_t *fm,
                                             bool wifi_online,
                                             bool cell_online) {
    if (!fm) return WAN_LINK_OFFLINE;
    edgehmi_wan_link_t prev = fm->active_link;

    if (fm->policy == NET_POLICY_WIFI_ONLY) {
        fm->active_link = wifi_online ? WAN_LINK_WIFI : WAN_LINK_OFFLINE;
    } else if (fm->policy == NET_POLICY_CELLULAR_ONLY) {
        fm->active_link = cell_online ? WAN_LINK_CELLULAR : WAN_LINK_OFFLINE;
    } else if (fm->policy == NET_POLICY_WIFI_PRIMARY) {
        if (wifi_online) {
            fm->wifi_fail_streak = 0;
            if (fm->wifi_ok_streak < 255) fm->wifi_ok_streak++;
            if (fm->active_link == WAN_LINK_OFFLINE || fm->active_link == WAN_LINK_WIFI) {
                fm->active_link = WAN_LINK_WIFI;
            } else if (fm->active_link == WAN_LINK_CELLULAR &&
                       fm->wifi_ok_streak >= fm->failback_threshold) {
                fm->active_link = WAN_LINK_WIFI;
            }
        } else {
            fm->wifi_ok_streak = 0;
            if (fm->wifi_fail_streak < 255) fm->wifi_fail_streak++;
            if (cell_online && (fm->active_link == WAN_LINK_OFFLINE ||
                                fm->wifi_fail_streak >= fm->failover_threshold)) {
                fm->active_link = WAN_LINK_CELLULAR;
            } else if (!cell_online) {
                fm->active_link = WAN_LINK_OFFLINE;
            }
        }
    } else { /* NET_POLICY_CELLULAR_PRIMARY */
        if (cell_online) {
            fm->cell_fail_streak = 0;
            fm->active_link = WAN_LINK_CELLULAR;
        } else {
            if (fm->cell_fail_streak < 255) fm->cell_fail_streak++;
            if (wifi_online && (fm->active_link == WAN_LINK_OFFLINE ||
                                fm->cell_fail_streak >= fm->failover_threshold)) {
                fm->active_link = WAN_LINK_WIFI;
            } else if (!wifi_online) {
                fm->active_link = WAN_LINK_OFFLINE;
            }
        }
    }

    if (fm->active_link != prev && fm->active_link != WAN_LINK_OFFLINE) {
        fm->switch_count++;
    }
    return fm->active_link;
}
