#ifndef EDGEHMI_CELLULAR_PROTO_H
#define EDGEHMI_CELLULAR_PROTO_H

#include "edgehmi_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CELL_RAT_NONE = 0,
    CELL_RAT_2G_3G,
    CELL_RAT_4G_CAT1,
    CELL_RAT_4G_LTE,
    CELL_RAT_5G_REDCAP,
    CELL_RAT_5G_NSA,
    CELL_RAT_5G_SA
} edgehmi_cell_rat_t;

typedef enum {
    WAN_LINK_OFFLINE = 0,
    WAN_LINK_WIFI = 1,
    WAN_LINK_CELLULAR = 2
} edgehmi_wan_link_t;

typedef struct {
    bool sim_ready;
    bool registered;
    bool ppp_up;
    int8_t rssi_dbm;      /* -113..-51 dBm, 0 if unknown */
    uint8_t signal_bars;  /* 0..4 */
    edgehmi_cell_rat_t rat;
    char operator_name[32];
    char iccid[24];
    uint32_t reconnect_count;
    uint32_t hw_reset_count;
} edgehmi_cell_status_t;

typedef struct {
    edgehmi_net_policy_t policy;
    edgehmi_wan_link_t active_link;
    uint8_t wifi_fail_streak;
    uint8_t wifi_ok_streak;
    uint8_t cell_fail_streak;
    uint8_t failover_threshold;  /* Consecutive failures before switching to backup (default 3) */
    uint8_t failback_threshold;  /* Consecutive primary recoveries before switching back (default 5) */
    uint32_t switch_count;
} edgehmi_wan_failover_t;

/* 3GPP AT Response Parsers (4G Cat.1/Cat.4 & 5G RedCap/NR) */
bool edgehmi_cell_parse_csq(const char *resp, int8_t *out_dbm, uint8_t *out_bars);
bool edgehmi_cell_parse_reg(const char *resp, bool *out_registered);
bool edgehmi_cell_parse_nwinfo(const char *resp, edgehmi_cell_rat_t *out_rat, char *out_oper, size_t oper_cap);
bool edgehmi_cell_parse_iccid(const char *resp, char *out_iccid, size_t iccid_cap);
const char *edgehmi_cell_rat_str(edgehmi_cell_rat_t rat);

/* Build 3GPP PDP context setup command (AT+CGDCONT=1,"IP","<apn>") */
size_t edgehmi_cell_build_apn_cmd(const char *apn, char *out_buf, size_t out_cap);

/* Build 4G/5G RAT preference AT command (Quectel/Fibocom/AirM2M compatible) */
size_t edgehmi_cell_build_rat_cmd(edgehmi_cell_gen_t gen, char *out_buf, size_t out_cap);

/* Deterministic Wi-Fi <-> 4G/5G multi-link failover state machine */
void edgehmi_wan_failover_init(edgehmi_wan_failover_t *fm, edgehmi_net_policy_t policy);
edgehmi_wan_link_t edgehmi_wan_failover_tick(edgehmi_wan_failover_t *fm,
                                             bool wifi_online,
                                             bool cell_online);

#ifdef __cplusplus
}
#endif

#endif /* EDGEHMI_CELLULAR_PROTO_H */
