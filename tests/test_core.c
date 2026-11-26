#include "config_loader.h"
#include "data_bus.h"
#include "discovery_proto.h"
#include "modbus_rtu.h"
#include "ota_package.h"
#include "rule_engine.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_alarm_raised = 0;
static int g_alarm_cleared = 0;

static void test_alarm_cb(const edgehmi_alarm_event_t *evt, void *ctx) {
    (void)ctx;
    if (evt->active) {
        g_alarm_raised++;
    } else {
        g_alarm_cleared++;
    }
}

static char *read_file_all(const char *path) {
    FILE *f = fopen(path, "rb");
    assert(f != NULL);
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    assert(sz > 0);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)sz + 1);
    assert(buf != NULL);
    size_t n = fread(buf, 1, (size_t)sz, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

static void test_data_bus(void) {
    edgehmi_bus_t bus;
    edgehmi_bus_init(&bus, NULL, NULL, NULL);

    int16_t ro_idx = -1, rw_idx = -1;
    assert(edgehmi_bus_register(&bus, "temp", TAG_TYPE_I16, TAG_ACCESS_R, "C", 20.0, &ro_idx) == EDGEHMI_OK);
    assert(edgehmi_bus_register(&bus, "pump", TAG_TYPE_BOOL, TAG_ACCESS_RW, "", 0.0, &rw_idx) == EDGEHMI_OK);
    assert(ro_idx == 0 && rw_idx == 1);
    assert(edgehmi_bus_find(&bus, "pump") == 1);
    assert(edgehmi_bus_find(&bus, "missing") == -1);

    assert(edgehmi_bus_request_write(&bus, ro_idx, 99.0, 100) == EDGEHMI_ERR_READONLY);
    assert(edgehmi_bus_request_write(&bus, rw_idx, 1.0, 100) == EDGEHMI_OK);
    double wv = 0.0;
    assert(edgehmi_bus_pop_write(&bus, rw_idx, &wv) == true);
    assert(fabs(wv - 1.0) < 1e-9);
    assert(edgehmi_bus_pop_write(&bus, rw_idx, &wv) == false);

    assert(edgehmi_bus_consume_ui_dirty(&bus, ro_idx) == true);
    assert(edgehmi_bus_consume_ui_dirty(&bus, ro_idx) == false);
    assert(edgehmi_bus_set(&bus, ro_idx, 25.5, TAG_QUAL_GOOD, 1000) == EDGEHMI_OK);
    assert(edgehmi_bus_consume_ui_dirty(&bus, ro_idx) == true);

    assert(edgehmi_bus_check_stale(&bus, 2500, 1000) == 2);
    double v = 0.0;
    edgehmi_quality_t q = TAG_QUAL_INIT;
    assert(edgehmi_bus_get(&bus, ro_idx, &v, &q) == EDGEHMI_OK);
    assert(q == TAG_QUAL_STALE);

    /* Multi-widget non-destructive snapshot version sync */
    int16_t wo_idx = -1;
    assert(edgehmi_bus_register(&bus, "cmd_only", TAG_TYPE_BOOL, TAG_ACCESS_W, "", 0.0, &wo_idx) == EDGEHMI_OK);
    uint32_t v1 = 0, v2 = 0;
    assert(edgehmi_bus_set(&bus, ro_idx, 31.2, TAG_QUAL_GOOD, 3000) == EDGEHMI_OK);
    assert(edgehmi_bus_get_snapshot(&bus, ro_idx, &v, &q, &v1) == EDGEHMI_OK);
    assert(edgehmi_bus_get_snapshot(&bus, ro_idx, &v, &q, &v2) == EDGEHMI_OK);
    assert(v1 == v2 && v1 > 1); /* Both widgets see identical bumped version */

    /* Write-only tag must NOT be marked STALE by check_stale */
    assert(edgehmi_bus_request_write(&bus, wo_idx, 1.0, 3000) == EDGEHMI_OK);
    assert(edgehmi_bus_check_stale(&bus, 9000, 1000) == 1); /* Only ro_idx goes stale, NOT wo_idx */
    assert(edgehmi_bus_get(&bus, wo_idx, &v, &q) == EDGEHMI_OK && q == TAG_QUAL_GOOD);

    /* Boot-time RS485 disconnect quality marking: INIT -> BAD bumps version */
    int16_t boot_idx = -1;
    assert(edgehmi_bus_register(&bus, "boot_sensor", TAG_TYPE_U16, TAG_ACCESS_R, "%", 0.0, &boot_idx) == EDGEHMI_OK);
    uint32_t bv0 = 0, bv1 = 0;
    assert(edgehmi_bus_get_snapshot(&bus, boot_idx, &v, &q, &bv0) == EDGEHMI_OK && q == TAG_QUAL_INIT);
    assert(edgehmi_bus_set_quality(&bus, boot_idx, TAG_QUAL_BAD, 4000) == EDGEHMI_OK);
    assert(edgehmi_bus_get_snapshot(&bus, boot_idx, &v, &q, &bv1) == EDGEHMI_OK && q == TAG_QUAL_BAD && bv1 > bv0);

    printf("  [PASS] test_data_bus (multi-widget version + write-only stale guard + boot BAD)\n");
}

static void test_modbus_rtu_and_byte_orders(void) {
    uint8_t req[16];
    assert(edgehmi_mb_build_read_req(1, 3, 0x0000, 10, req, sizeof(req)) == 8);
    assert(req[6] == 0xC5 && req[7] == 0xCD);

    assert(edgehmi_mb_build_write_single(1, 5, 1, 1, req, sizeof(req)) == 8);
    assert(req[4] == 0xFF && req[5] == 0x00);

    /* Negative i16 round-trip */
    edgehmi_mb_point_t pt;
    memset(&pt, 0, sizeof(pt));
    pt.slave_id = 1;
    pt.func_code = 3;
    pt.reg_addr = 100;
    pt.type = TAG_TYPE_I16;
    pt.scale = 0.1;

    uint16_t neg_raw = edgehmi_mb_eng_to_raw16(&pt, -12.5);
    uint8_t resp[7] = { 1, 3, 2, (uint8_t)(neg_raw >> 8), (uint8_t)(neg_raw & 0xFF), 0, 0 };
    uint16_t crc = edgehmi_mb_crc16(resp, 5);
    resp[5] = (uint8_t)(crc & 0xFF);
    resp[6] = (uint8_t)(crc >> 8);
    double eng = 0.0;
    assert(edgehmi_mb_parse_read_resp(&pt, resp, sizeof(resp), &eng) == EDGEHMI_OK);
    assert(fabs(eng - (-12.5)) < 1e-6);

    resp[6] ^= 0xFF;
    assert(edgehmi_mb_parse_read_resp(&pt, resp, sizeof(resp), &eng) == EDGEHMI_ERR_CRC);

    /* Test all 4 32-bit byte orders (ABCD, CDAB, BADC, DCBA) via FC16 encode + FC03 decode */
    edgehmi_byte_order_t orders[4] = {
        BYTE_ORDER_ABCD, BYTE_ORDER_CDAB, BYTE_ORDER_BADC, BYTE_ORDER_DCBA
    };
    pt.type = TAG_TYPE_F32;
    pt.scale = 1.0;
    for (int oi = 0; oi < 4; ++oi) {
        pt.byte_order = orders[oi];
        uint8_t fc16[13];
        assert(edgehmi_mb_build_write_multi(&pt, 1234.5, fc16, sizeof(fc16)) == 13);
        assert(fc16[1] == 16 && fc16[5] == 2 && fc16[6] == 4);
        /* Verify FC16 CRC */
        uint16_t wcrc = edgehmi_mb_crc16(fc16, 11);
        assert(fc16[11] == (uint8_t)(wcrc & 0xFF) && fc16[12] == (uint8_t)(wcrc >> 8));

        /* Wrap the 4 encoded payload bytes back into an FC03 read response */
        uint8_t fresp[9] = { 1, 3, 4, fc16[7], fc16[8], fc16[9], fc16[10], 0, 0 };
        uint16_t rcrc = edgehmi_mb_crc16(fresp, 7);
        fresp[7] = (uint8_t)(rcrc & 0xFF);
        fresp[8] = (uint8_t)(rcrc >> 8);
        assert(edgehmi_mb_parse_read_resp(&pt, fresp, sizeof(fresp), &eng) == EDGEHMI_OK);
        assert(fabs(eng - 1234.5) < 1e-4);
    }

    printf("  [PASS] test_modbus_rtu_and_byte_orders (ABCD/CDAB/BADC/DCBA + FC16)\n");
}

static void test_rule_engine_hysteresis_and_debounce(void) {
    edgehmi_bus_t bus;
    edgehmi_bus_init(&bus, NULL, NULL, NULL);
    int16_t lvl = -1, pump = -1;
    edgehmi_bus_register(&bus, "tank_level", TAG_TYPE_U16, TAG_ACCESS_R, "%", 50.0, &lvl);
    edgehmi_bus_register(&bus, "pump_cmd", TAG_TYPE_BOOL, TAG_ACCESS_RW, "", 1.0, &pump);

    edgehmi_rule_t rules[2];
    memset(rules, 0, sizeof(rules));

    strcpy(rules[0].id, "high_stop");
    rules[0].enabled = true;
    rules[0].logic_mode = RULE_LOGIC_ALL;
    rules[0].cond_count = 1;
    rules[0].conds[0].tag_idx = lvl;
    rules[0].conds[0].op = RULE_OP_GE;
    rules[0].conds[0].threshold = 90.0;
    rules[0].conds[0].hysteresis = 3.0;
    rules[0].debounce_ms = 500;
    rules[0].action = RULE_ACT_SET_TAG;
    rules[0].target_tag_idx = pump;
    rules[0].action_value = 0.0;

    strcpy(rules[1].id, "high_alarm");
    rules[1].enabled = true;
    rules[1].logic_mode = RULE_LOGIC_ALL;
    rules[1].cond_count = 1;
    rules[1].conds[0].tag_idx = lvl;
    rules[1].conds[0].op = RULE_OP_GE;
    rules[1].conds[0].threshold = 90.0;
    rules[1].conds[0].hysteresis = 3.0;
    rules[1].debounce_ms = 0;
    rules[1].action = RULE_ACT_ALARM;
    rules[1].alarm_level = ALARM_CRITICAL;
    strcpy(rules[1].alarm_msg, "High level!");

    edgehmi_rule_engine_t eng;
    g_alarm_raised = 0;
    g_alarm_cleared = 0;
    edgehmi_rules_init(&eng, rules, 2, test_alarm_cb, NULL);

    edgehmi_bus_set(&bus, lvl, 95.0, TAG_QUAL_BAD, 1000);
    assert(edgehmi_rules_eval(&eng, &bus, 1000) == 0);

    edgehmi_bus_set(&bus, lvl, 91.0, TAG_QUAL_GOOD, 2000);
    assert(edgehmi_rules_eval(&eng, &bus, 2000) == 1);
    assert(g_alarm_raised == 1);
    assert(rules[0].active == false && rules[0].pending_trigger == true);

    assert(edgehmi_rules_eval(&eng, &bus, 2400) == 0);
    assert(edgehmi_rules_eval(&eng, &bus, 2500) == 1);
    assert(rules[0].active == true);
    double pval = 1.0;
    assert(edgehmi_bus_pop_write(&bus, pump, &pval) == true && fabs(pval) < 1e-9);

    edgehmi_bus_set(&bus, lvl, 88.0, TAG_QUAL_GOOD, 3000);
    edgehmi_rules_eval(&eng, &bus, 3000);
    assert(rules[0].active == true && g_alarm_cleared == 0);

    edgehmi_bus_set(&bus, lvl, 86.5, TAG_QUAL_GOOD, 3500);
    edgehmi_rules_eval(&eng, &bus, 3500);
    assert(rules[0].active == false && g_alarm_cleared == 1);

    printf("  [PASS] test_rule_engine_hysteresis_and_debounce\n");
}

static void test_compound_and_comm_loss_rules(void) {
    char *raw = read_file_all("configs/pump_station.json");
    edgehmi_config_t *cfg = (edgehmi_config_t *)calloc(1, sizeof(edgehmi_config_t));
    edgehmi_bus_t bus;
    edgehmi_bus_init(&bus, NULL, NULL, NULL);
    char err[128] = {0};
    assert(edgehmi_config_load_json(raw, cfg, &bus, err, sizeof(err)) == EDGEHMI_OK);
    assert(strcmp(cfg->api_token, "edgehmi-secret-2026") == 0);

    edgehmi_rule_engine_t eng;
    g_alarm_raised = 0;
    g_alarm_cleared = 0;
    edgehmi_rules_init(&eng, cfg->rules, cfg->rule_count, test_alarm_cb, NULL);

    int16_t idx_lvl  = edgehmi_bus_find(&bus, "tank_level");
    int16_t idx_auto = edgehmi_bus_find(&bus, "auto_mode");
    int16_t idx_pump = edgehmi_bus_find(&bus, "pump_cmd");
    int16_t idx_curr = edgehmi_bus_find(&bus, "motor_curr");

    /* 1. Compound AND test: auto_mode=0 + tank_level=30 must NOT auto-start pump */
    edgehmi_bus_set(&bus, idx_pump, 0.0, TAG_QUAL_GOOD, 1000);
    edgehmi_bus_set(&bus, idx_curr, 0.1, TAG_QUAL_GOOD, 1000);
    edgehmi_bus_set(&bus, idx_auto, 0.0, TAG_QUAL_GOOD, 1000);
    edgehmi_bus_set(&bus, idx_lvl,  30.0, TAG_QUAL_GOOD, 1000);
    edgehmi_rules_eval(&eng, &bus, 1000);
    edgehmi_rules_eval(&eng, &bus, 1500);
    double wv = -1.0;
    assert(edgehmi_bus_pop_write(&bus, idx_pump, &wv) == false);

    /* 2. Enable auto_mode=1 -> after 400ms debounce, compound AND triggers pump_cmd=1 */
    edgehmi_bus_set(&bus, idx_auto, 1.0, TAG_QUAL_GOOD, 2000);
    edgehmi_rules_eval(&eng, &bus, 2000);
    edgehmi_rules_eval(&eng, &bus, 2450);
    assert(edgehmi_bus_pop_write(&bus, idx_pump, &wv) == true && fabs(wv - 1.0) < 1e-6);

    /* 3. RS485 Comm-Loss Fail-Safe: tank_level goes STALE -> after 300ms trips pump_cmd=0 & raises alarm */
    edgehmi_bus_set(&bus, idx_lvl, 50.0, TAG_QUAL_STALE, 3000);
    edgehmi_rules_eval(&eng, &bus, 3000);
    edgehmi_rules_eval(&eng, &bus, 3350);
    assert(edgehmi_bus_pop_write(&bus, idx_pump, &wv) == true && fabs(wv) < 1e-6);
    assert(g_alarm_raised >= 1);

    /* 4. Continuous enforce safety interlock: operator tries to restart pump while comm_loss still active */
    assert(edgehmi_bus_request_write(&bus, idx_pump, 1.0, 3500) == EDGEHMI_OK);
    assert(edgehmi_bus_pop_write(&bus, idx_pump, &wv) == true && fabs(wv - 1.0) < 1e-6);
    /* Next eval tick must immediately re-trip pump_cmd back to 0 */
    assert(edgehmi_rules_eval(&eng, &bus, 3600) >= 1);
    assert(edgehmi_bus_pop_write(&bus, idx_pump, &wv) == true && fabs(wv) < 1e-6);

    free(cfg);
    free(raw);
    printf("  [PASS] test_compound_and_comm_loss_rules (including continuous enforce interlock)\n");
}

static void test_modbus_block_read_with_cdab_float(void) {
    char *raw = read_file_all("configs/pump_station.json");
    edgehmi_config_t *cfg = (edgehmi_config_t *)calloc(1, sizeof(edgehmi_config_t));
    edgehmi_bus_t bus;
    edgehmi_bus_init(&bus, NULL, NULL, NULL);
    char err[128] = {0};
    assert(edgehmi_config_load_json(raw, cfg, &bus, err, sizeof(err)) == EDGEHMI_OK);

    edgehmi_mb_block_t blocks[EDGEHMI_MAX_MODBUS_BLOCKS];
    size_t nblk = edgehmi_mb_plan_blocks(
        cfg->points, cfg->point_count, blocks, EDGEHMI_MAX_MODBUS_BLOCKS, 4, 32);
    /* 10 points -> 4 blocks; FC3 100..106 holds 6 points (5x16bit + 1x32bit CDAB float = 7 regs) */
    assert(nblk == 4);

    const edgehmi_mb_block_t *reg_blk = NULL;
    for (size_t i = 0; i < nblk; ++i) {
        if (blocks[i].func_code == 3 && blocks[i].start_addr == 100) reg_blk = &blocks[i];
    }
    assert(reg_blk != NULL && reg_blk->quantity == 7 && reg_blk->point_count == 6);

    /* Encode float32 1248.5f in CDAB word-swapped order into regs 105..106 */
    float fflow = 1248.5f;
    uint32_t fbits = 0;
    memcpy(&fbits, &fflow, sizeof(fbits));
    uint16_t regs[7] = {
        725u, 1480u, (uint16_t)(-15), 132u, 41u,
        (uint16_t)(fbits & 0xFFFFu),        /* CD (low word first) */
        (uint16_t)((fbits >> 16) & 0xFFFFu) /* AB (high word second) */
    };

    uint8_t frame[3 + 14 + 2];
    frame[0] = 1;
    frame[1] = 3;
    frame[2] = 14;
    for (int i = 0; i < 7; ++i) {
        frame[3 + i * 2]     = (uint8_t)(regs[i] >> 8);
        frame[3 + i * 2 + 1] = (uint8_t)(regs[i] & 0xFFu);
    }
    uint16_t crc = edgehmi_mb_crc16(frame, 17);
    frame[17] = (uint8_t)(crc & 0xFFu);
    frame[18] = (uint8_t)(crc >> 8);

    assert(edgehmi_mb_parse_block_resp(reg_blk, cfg->points, frame, sizeof(frame), &bus, 5000) == EDGEHMI_OK);

    double v = 0.0;
    edgehmi_quality_t q = TAG_QUAL_INIT;
    assert(edgehmi_bus_get(&bus, edgehmi_bus_find(&bus, "total_flow_m3"), &v, &q) == EDGEHMI_OK);
    assert(q == TAG_QUAL_GOOD && fabs(v - 1248.5) < 1e-4);

    free(cfg);
    free(raw);
    printf("  [PASS] test_modbus_block_read_with_cdab_float\n");
}

static void test_sha256_and_epk_package(void) {
    /* Official NIST FIPS 180-4 SHA-256 test vector for "abc" */
    uint8_t digest[32];
    char hex[65];
    edgehmi_sha256((const uint8_t *)"abc", 3, digest);
    edgehmi_sha256_to_hex(digest, hex);
    assert(strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0);

    /* Pack & verify .epk firmware package */
    const uint8_t fw_payload[] = "EDGEHMI-FW-BINARY-TEST-PAYLOAD-1234567890";
    uint8_t epk_buf[256];
    size_t total = edgehmi_epk_pack(
        EPK_TYPE_FIRMWARE, "1.2.5", fw_payload, sizeof(fw_payload) - 1, epk_buf, sizeof(epk_buf));
    assert(total == EDGEHMI_EPK_HEADER_SIZE + sizeof(fw_payload) - 1);

    edgehmi_epk_header_t hdr;
    const uint8_t *out_payload = NULL;
    assert(edgehmi_epk_verify(epk_buf, total, &hdr, &out_payload) == EDGEHMI_OK);
    assert(hdr.pkg_type == EPK_TYPE_FIRMWARE);
    assert(strcmp(hdr.version, "1.2.5") == 0);
    assert(hdr.payload_len == sizeof(fw_payload) - 1);
    assert(memcmp(out_payload, fw_payload, hdr.payload_len) == 0);

    /* Corrupt 1 bit in payload -> must fail SHA-256 verification */
    epk_buf[EDGEHMI_EPK_HEADER_SIZE + 2] ^= 0x01u;
    assert(edgehmi_epk_verify(epk_buf, total, &hdr, &out_payload) == EDGEHMI_ERR_CRC);
    epk_buf[EDGEHMI_EPK_HEADER_SIZE + 2] ^= 0x01u;

    /* Corrupt 1 bit in header -> must fail header CRC32 verification */
    epk_buf[10] ^= 0x01u;
    assert(edgehmi_epk_verify(epk_buf, total, &hdr, &out_payload) == EDGEHMI_ERR_CRC);

    printf("  [PASS] test_sha256_and_epk_package (NIST SHA-256 + EPK1 tamper detection)\n");
}

static void test_discovery_protocol(void) {
    const char *probe = "{\"cmd\":\"discover\",\"proto\":1}";
    assert(edgehmi_discovery_is_probe(probe, strlen(probe)) == true);
    assert(edgehmi_discovery_is_probe("{\"cmd\":\"other\"}", 15) == false);
    assert(edgehmi_discovery_is_probe("not json", 8) == false);

    edgehmi_device_info_t info;
    memset(&info, 0, sizeof(info));
    strcpy(info.device_id, "edgehmi-a1b2c3");
    strcpy(info.name, "AQUACONTROL HMI / 01");
    strcpy(info.fw_version, "1.1.0");
    strcpy(info.cfg_version, "1.0.0");
    info.http_port = 80;
    info.width = 800;
    info.height = 480;
    info.comm_loss = false;
    info.uptime_s = 3600;

    char buf[384];
    size_t n = edgehmi_discovery_build_announce(&info, buf, sizeof(buf));
    assert(n > 0);
    assert(strstr(buf, "\"type\":\"edgehmi_device\"") != NULL);
    assert(strstr(buf, "\"id\":\"edgehmi-a1b2c3\"") != NULL);
    assert(strstr(buf, "\"fw_ver\":\"1.1.0\"") != NULL);
    assert(strstr(buf, "\"comm_loss\":false") != NULL);

    /* JSON escaping in discovery announce when device name contains quotes/backslashes */
    strcpy(info.name, "Pump \"A\" \\ Line 1");
    size_t n2 = edgehmi_discovery_build_announce(&info, buf, sizeof(buf));
    assert(n2 > 0);
    assert(strstr(buf, "Pump \\\"A\\\" \\\\ Line 1") != NULL);

    printf("  [PASS] test_discovery_protocol (including JSON escaping)\n");
}

static void test_security_and_nvs_keys(void) {
    /* Constant-time string compare */
    assert(edgehmi_ct_str_eq("Bearer edgehmi-secret-2026", "Bearer edgehmi-secret-2026") == true);
    assert(edgehmi_ct_str_eq("Bearer edgehmi-secret-2026", "Bearer edgehmi-secret-2027") == false);
    assert(edgehmi_ct_str_eq("1234", "12345") == false);
    assert(edgehmi_ct_str_eq(NULL, "1234") == false);

    /* NVS 15-char safe key collision avoidance */
    char k_short[16], k_long1[16], k_long2[16];
    edgehmi_nvs_safe_key("target_level", k_short);
    assert(strcmp(k_short, "target_level") == 0);

    edgehmi_nvs_safe_key("solar_inverter_limit_active_w", k_long1);
    edgehmi_nvs_safe_key("solar_inverter_limit_reactive", k_long2);
    assert(strlen(k_long1) == 15 && strlen(k_long2) == 15);
    assert(strcmp(k_long1, k_long2) != 0);

    printf("  [PASS] test_security_and_nvs_keys (constant-time eq + collision-free NVS keys)\n");
}

int main(void) {
    printf("Running EdgeHMI Full C11 Core Unit Tests...\n");
    test_data_bus();
    test_modbus_rtu_and_byte_orders();
    test_rule_engine_hysteresis_and_debounce();
    test_compound_and_comm_loss_rules();
    test_modbus_block_read_with_cdab_float();
    test_sha256_and_epk_package();
    test_discovery_protocol();
    test_security_and_nvs_keys();
    printf("ALL FULL C11 CORE TESTS PASSED.\n");
    return 0;
}
