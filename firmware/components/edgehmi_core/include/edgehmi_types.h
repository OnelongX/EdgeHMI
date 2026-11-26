#ifndef EDGEHMI_TYPES_H
#define EDGEHMI_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Compile-time static capacities — zero runtime heap fragmentation */
#define EDGEHMI_MAX_TAGS            128
#define EDGEHMI_MAX_MODBUS_POINTS   128
#define EDGEHMI_MAX_MODBUS_BLOCKS   32
#define EDGEHMI_MAX_LOCAL_IO        24
#define EDGEHMI_MAX_CAN_SIGNALS     32
#define EDGEHMI_MAX_RULES           64
#define EDGEHMI_MAX_RULE_CONDS      4
#define EDGEHMI_MAX_PAGES           8
#define EDGEHMI_MAX_WIDGETS_PER_PG  48
#define EDGEHMI_MAX_NAME_LEN        32
#define EDGEHMI_MAX_TEXT_LEN        96
#define EDGEHMI_MAX_UNIT_LEN        16
#define EDGEHMI_MAX_ALARMS          32
#define EDGEHMI_MAX_HISTORY_SAMPLES 256
#define EDGEHMI_MAX_HISTORY_TAGS    8

typedef enum {
    EDGEHMI_OK = 0,
    EDGEHMI_ERR_INVALID_ARG = -1,
    EDGEHMI_ERR_FULL = -2,
    EDGEHMI_ERR_NOT_FOUND = -3,
    EDGEHMI_ERR_PARSE = -4,
    EDGEHMI_ERR_SCHEMA = -5,
    EDGEHMI_ERR_CRC = -6,
    EDGEHMI_ERR_IO = -7,
    EDGEHMI_ERR_READONLY = -8
} edgehmi_err_t;

typedef enum {
    TAG_TYPE_BOOL = 0,
    TAG_TYPE_U16,
    TAG_TYPE_I16,
    TAG_TYPE_U32,
    TAG_TYPE_F32
} edgehmi_tag_type_t;

typedef enum {
    TAG_QUAL_INIT = 0,
    TAG_QUAL_GOOD = 1,
    TAG_QUAL_BAD = 2,
    TAG_QUAL_STALE = 3
} edgehmi_quality_t;

typedef enum {
    TAG_ACCESS_R = 1,
    TAG_ACCESS_W = 2,
    TAG_ACCESS_RW = 3
} edgehmi_access_t;

typedef struct {
    char name[EDGEHMI_MAX_NAME_LEN];
    char unit[EDGEHMI_MAX_UNIT_LEN];
    edgehmi_tag_type_t type;
    edgehmi_access_t access;
    double value;
    double pending_write;
    bool has_pending_write;
    edgehmi_quality_t quality;
    uint64_t updated_ms;
    uint32_t version;
    bool dirty_ui;
} edgehmi_tag_t;

typedef enum {
    BYTE_ORDER_ABCD = 0, /* Big-endian standard (AB CD) */
    BYTE_ORDER_CDAB = 1, /* Mid-little-endian word-swap (CD AB) — common on Chinese meters/inverters */
    BYTE_ORDER_BADC = 2, /* Mid-big-endian byte-swap (BA DC) */
    BYTE_ORDER_DCBA = 3  /* Little-endian (DC BA) */
} edgehmi_byte_order_t;

typedef struct {
    char tag_name[EDGEHMI_MAX_NAME_LEN];
    int16_t tag_idx;
    uint8_t slave_id;
    uint8_t func_code;
    uint16_t reg_addr;
    edgehmi_tag_type_t type;
    edgehmi_byte_order_t byte_order;
    edgehmi_access_t access;
    double scale;
    double offset;
    double default_val;
    bool retentive;
    char unit[EDGEHMI_MAX_UNIT_LEN];
} edgehmi_mb_point_t;

typedef struct {
    uint32_t tx_count;
    uint32_t rx_ok;
    uint32_t crc_err;
    uint32_t timeout_err;
    uint32_t retry_count;
    uint32_t last_rtt_ms;
} edgehmi_mb_stats_t;

typedef struct {
    uint8_t slave_id;
    uint8_t func_code;
    uint16_t start_addr;
    uint16_t quantity;
    uint16_t point_indices[32];
    size_t point_count;
} edgehmi_mb_block_t;

typedef enum {
    RULE_OP_GT = 0,
    RULE_OP_GE,
    RULE_OP_LT,
    RULE_OP_LE,
    RULE_OP_EQ,
    RULE_OP_NE
} edgehmi_rule_op_t;

typedef enum {
    RULE_ACT_SET_TAG = 0,
    RULE_ACT_ALARM
} edgehmi_rule_action_t;

typedef enum {
    ALARM_INFO = 0,
    ALARM_WARN = 1,
    ALARM_CRITICAL = 2
} edgehmi_alarm_level_t;

typedef enum {
    RULE_LOGIC_ALL = 0,      /* Single condition or AND of all conditions */
    RULE_LOGIC_ANY = 1,      /* OR of conditions */
    RULE_LOGIC_COMM_LOSS = 2 /* Fail-Safe trigger on TAG_QUAL_STALE / TAG_QUAL_BAD */
} edgehmi_rule_logic_t;

typedef struct {
    char tag[EDGEHMI_MAX_NAME_LEN];
    int16_t tag_idx;
    edgehmi_rule_op_t op;
    double threshold;
    double hysteresis;
} edgehmi_rule_cond_t;

typedef struct {
    char id[EDGEHMI_MAX_NAME_LEN];
    char desc[EDGEHMI_MAX_TEXT_LEN];
    bool enabled;

    /* Condition(s) */
    edgehmi_rule_logic_t logic_mode;
    edgehmi_rule_cond_t conds[EDGEHMI_MAX_RULE_CONDS];
    size_t cond_count;
    uint32_t debounce_ms;

    /* Action */
    edgehmi_rule_action_t action;
    char target_tag[EDGEHMI_MAX_NAME_LEN];
    int16_t target_tag_idx;
    double action_value;
    bool enforce; /* Continuous safety interlock: re-assert action_value if overridden while active */
    edgehmi_alarm_level_t alarm_level;
    char alarm_msg[EDGEHMI_MAX_TEXT_LEN];

    /* Runtime deterministic state */
    bool active;
    bool pending_trigger;
    uint64_t pending_since_ms;
    uint32_t trigger_count;
} edgehmi_rule_t;

typedef enum {
    WIDGET_CARD = 0,
    WIDGET_LABEL,
    WIDGET_VALUE,
    WIDGET_GAUGE,
    WIDGET_BAR,
    WIDGET_BUTTON,
    WIDGET_SWITCH,
    WIDGET_BADGE,
    WIDGET_CHART,
    WIDGET_SLIDER,
    WIDGET_SPINBOX,
    WIDGET_ALARM_LIST,
    WIDGET_QRCODE,
    WIDGET_IMAGE
} edgehmi_widget_type_t;

typedef struct {
    char id[EDGEHMI_MAX_NAME_LEN];
    edgehmi_widget_type_t type;
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;
    char text[EDGEHMI_MAX_TEXT_LEN];
    char bind_tag[EDGEHMI_MAX_NAME_LEN];
    int16_t bind_tag_idx;
    char unit[EDGEHMI_MAX_UNIT_LEN];
    double min_val;
    double max_val;
    double step;
    uint8_t decimals;
    uint32_t color_rgb;
    bool has_write_value;
    double write_value;
    char nav_page[EDGEHMI_MAX_NAME_LEN];
    char true_text[EDGEHMI_MAX_NAME_LEN];
    char false_text[EDGEHMI_MAX_NAME_LEN];
    char src[EDGEHMI_MAX_TEXT_LEN];
    bool confirm;
    char confirm_text[EDGEHMI_MAX_TEXT_LEN];
    bool pin_required;
} edgehmi_widget_t;

typedef enum {
    LOCAL_IO_DI = 0,
    LOCAL_IO_DO = 1,
    LOCAL_IO_ADC = 2,
    LOCAL_IO_PWM = 3
} edgehmi_local_io_mode_t;

typedef struct {
    char tag_name[EDGEHMI_MAX_NAME_LEN];
    int16_t tag_idx;
    int8_t gpio_pin;
    edgehmi_local_io_mode_t mode;
    bool active_low;
    double scale;
    double offset;
    double default_val;
    bool retentive;
    char unit[EDGEHMI_MAX_UNIT_LEN];
} edgehmi_local_io_t;

typedef struct {
    bool enabled;
    char host[64];
    uint16_t port;
    uint32_t timeout_ms;
} edgehmi_mb_tcp_cfg_t;

typedef struct {
    char tag_name[EDGEHMI_MAX_NAME_LEN];
    int16_t tag_idx;
    uint32_t can_id;
    bool ext_id;
    uint8_t start_bit;
    uint8_t bit_len;
    bool is_signed;
    bool big_endian; /* false = Intel (little-endian), true = Motorola (big-endian) */
    double scale;
    double offset;
    double default_val;
    char unit[EDGEHMI_MAX_UNIT_LEN];
} edgehmi_can_signal_t;

typedef struct {
    bool enabled;
    uint32_t bitrate;
    int8_t tx_pin;
    int8_t rx_pin;
    edgehmi_can_signal_t signals[EDGEHMI_MAX_CAN_SIGNALS];
    size_t signal_count;
} edgehmi_can_cfg_t;

typedef struct {
    bool enabled;
    char mqtt_uri[96];
    char client_id[48];
    char username[32];
    char password[48];
    char topic_prefix[48];
    uint32_t publish_interval_ms;
} edgehmi_cloud_cfg_t;

typedef struct {
    char ssid[33];
    char password[65];
    bool ap_fallback;
    bool modem_4g_enabled;
    int8_t modem_tx_pin;
    int8_t modem_rx_pin;
} edgehmi_net_cfg_t;

typedef struct {
    char id[EDGEHMI_MAX_NAME_LEN];
    char title[48];
    edgehmi_widget_t widgets[EDGEHMI_MAX_WIDGETS_PER_PG];
    size_t widget_count;
} edgehmi_page_t;

typedef struct {
    char version[16];
    char device_name[48];
    uint16_t width;
    uint16_t height;
    bool dark_theme;
    uint32_t poll_interval_ms;
    uint32_t watchdog_timeout_s;
    char engineer_pin[16];
    char api_token[64];

    /* Modbus RTU bus settings */
    uint8_t mb_port;
    uint32_t mb_baudrate;
    int8_t mb_tx_pin;
    int8_t mb_rx_pin;
    int8_t mb_de_pin;
    uint32_t mb_timeout_ms;

    edgehmi_mb_point_t points[EDGEHMI_MAX_MODBUS_POINTS];
    size_t point_count;

    /* Extended industrial buses & native GPIO/ADC */
    edgehmi_mb_tcp_cfg_t modbus_tcp;
    edgehmi_local_io_t local_io[EDGEHMI_MAX_LOCAL_IO];
    size_t local_io_count;
    edgehmi_can_cfg_t can_bus;

    /* Network & Cloud MQTT telemetry */
    edgehmi_net_cfg_t net;
    edgehmi_cloud_cfg_t cloud;

    edgehmi_rule_t rules[EDGEHMI_MAX_RULES];
    size_t rule_count;

    edgehmi_page_t pages[EDGEHMI_MAX_PAGES];
    size_t page_count;
} edgehmi_config_t;

#ifdef __cplusplus
}
#endif

#endif /* EDGEHMI_TYPES_H */
