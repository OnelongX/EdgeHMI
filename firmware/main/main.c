#include "calc_engine.h"
#include "cloud_mqtt.h"
#include "config_loader.h"
#include "data_bus.h"
#include "discovery_service.h"
#include "edgehmi_bsp.h"
#include "fieldbus_ext.h"
#include "history_log.h"
#include "local_io.h"
#include "modbus_rtu.h"
#include "ota_config.h"
#include "ota_firmware.h"
#include "rule_engine.h"
#include "ui_engine_lvgl.h"

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <stdlib.h>
#include <string.h>

static const char *TAG = "edgehmi_main";

/* Static runtime instances — zero runtime fragmentation */
static edgehmi_config_t s_active_cfg;
static edgehmi_bus_t s_bus;
static edgehmi_rule_engine_t s_rules;
static edgehmi_history_t s_history;
static edgehmi_mb_block_t s_mb_blocks[EDGEHMI_MAX_MODBUS_BLOCKS];
static size_t s_mb_block_count = 0;
static edgehmi_mb_stats_t s_mb_stats;
static SemaphoreHandle_t s_bus_mutex = NULL;
static SemaphoreHandle_t s_cfg_mutex = NULL;
static volatile bool s_ui_reload_pending = false;

/* Track active RS485 UART driver state for empty-flash boot & hot-reload reconfig */
static bool s_uart_initialized = false;
static uint8_t s_uart_port = 0;
static uint32_t s_uart_baud = 0;
static int8_t s_uart_tx = -1;
static int8_t s_uart_rx = -1;
static int8_t s_uart_de = -1;

static void nvs_retentive_save(const char *tag_name, double val) {
    nvs_handle_t h;
    if (nvs_open("edgehmi_ret", NVS_READWRITE, &h) == ESP_OK) {
        char key[16];
        edgehmi_nvs_safe_key(tag_name, key);
        nvs_set_blob(h, key, &val, sizeof(val));
        nvs_commit(h);
        nvs_close(h);
    }
}

static bool nvs_retentive_load(const char *tag_name, double *out_val) {
    nvs_handle_t h;
    if (nvs_open("edgehmi_ret", NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    char key[16];
    edgehmi_nvs_safe_key(tag_name, key);
    size_t sz = sizeof(*out_val);
    bool ok = (nvs_get_blob(h, key, out_val, &sz) == ESP_OK && sz == sizeof(*out_val));
    nvs_close(h);
    return ok;
}

static void bus_lock_cb(void *ctx) {
    xSemaphoreTake((SemaphoreHandle_t)ctx, portMAX_DELAY);
}

static void bus_unlock_cb(void *ctx) {
    xSemaphoreGive((SemaphoreHandle_t)ctx);
}

static uint64_t now_ms(void) {
    return (uint64_t)(esp_timer_get_time() / 1000LL);
}

static void on_alarm_event(const edgehmi_alarm_event_t *evt, void *ctx) {
    (void)ctx;
    edgehmi_history_record_alarm(&s_history, evt);
    edgehmi_cloud_mqtt_publish_alarm(evt);
    ESP_LOGW(TAG, "[ALARM %s] rule=%s level=%d msg=%s",
             evt->active ? "RAISED" : "CLEARED",
             evt->rule_id, (int)evt->level, evt->message);
}

static void rs485_configure(const edgehmi_config_t *cfg) {
    if (!cfg || cfg->mb_baudrate == 0) {
        return;
    }
    uart_config_t uc = {
        .baud_rate = (int)cfg->mb_baudrate,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    if (s_uart_initialized && s_uart_port != cfg->mb_port) {
        uart_driver_delete(s_uart_port);
        s_uart_initialized = false;
    }

    if (!s_uart_initialized) {
        if (uart_driver_install(cfg->mb_port, 512, 512, 0, NULL, 0) != ESP_OK) {
            ESP_LOGE(TAG, "UART%u driver install failed", (unsigned)cfg->mb_port);
            return;
        }
        s_uart_initialized = true;
        s_uart_port = cfg->mb_port;
        s_uart_baud = 0;
        s_uart_tx = -1;
        s_uart_rx = -1;
        s_uart_de = -1;
    }

    if (s_uart_baud != cfg->mb_baudrate) {
        uart_param_config(cfg->mb_port, &uc);
        s_uart_baud = cfg->mb_baudrate;
    }
    if (s_uart_tx != cfg->mb_tx_pin || s_uart_rx != cfg->mb_rx_pin || s_uart_de != cfg->mb_de_pin) {
        uart_set_pin(cfg->mb_port, cfg->mb_tx_pin, cfg->mb_rx_pin, cfg->mb_de_pin, UART_PIN_NO_CHANGE);
        if (cfg->mb_de_pin >= 0) {
            uart_set_mode(cfg->mb_port, UART_MODE_RS485_HALF_DUPLEX);
        }
        s_uart_tx = cfg->mb_tx_pin;
        s_uart_rx = cfg->mb_rx_pin;
        s_uart_de = cfg->mb_de_pin;
    }
}

/* Core 1: Deterministic Industrial Control Task (Modbus + Interlock Rules) */
static void control_task(void *arg) {
    (void)arg;
    esp_task_wdt_add(NULL);

    uint8_t tx_buf[16];
    uint8_t rx_buf[64];

    for (;;) {
        xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
        uint32_t interval_ms = s_active_cfg.poll_interval_ms >= 50 ? s_active_cfg.poll_interval_ms : 250;

        /* 1. Flush pending writes from UI / Rule / Cloud to RS485 slaves + NVS */
        for (size_t i = 0; i < s_active_cfg.point_count; ++i) {
            const edgehmi_mb_point_t *pt = &s_active_cfg.points[i];
            double wval = 0.0;
            if (edgehmi_bus_pop_write(&s_bus, pt->tag_idx, &wval)) {
                if (pt->retentive) {
                    nvs_retentive_save(pt->tag_name, wval);
                }
                if (s_uart_initialized) {
                    size_t len = 0;
                    if (pt->type == TAG_TYPE_U32 || pt->type == TAG_TYPE_F32) {
                        len = edgehmi_mb_build_write_multi(pt, wval, tx_buf, sizeof(tx_buf));
                    } else {
                        /* Respect Holding Register (FC03/06) vs Coil (FC01/05) even for BOOL tags */
                        uint8_t wfc = (pt->func_code == 1 || pt->func_code == 5) ? 5 : 6;
                        uint16_t raw = edgehmi_mb_eng_to_raw16(pt, wval);
                        len = edgehmi_mb_build_write_single(
                            pt->slave_id, wfc, pt->reg_addr, raw, tx_buf, sizeof(tx_buf));
                    }
                    if (len > 0) {
                        s_mb_stats.tx_count++;
                        uart_flush_input(s_active_cfg.mb_port);
                        uart_write_bytes(s_active_cfg.mb_port, (const char *)tx_buf, len);
                        uart_read_bytes(s_active_cfg.mb_port, rx_buf, sizeof(rx_buf),
                                        pdMS_TO_TICKS(s_active_cfg.mb_timeout_ms));
                    }
                }
                esp_task_wdt_reset();
            }
        }

        /* 2. Deterministic Modbus RTU Block Read scan with retry, TWDT feeding & comm-loss marking */
        uint8_t blk_rx[128];
        if (s_uart_initialized) {
            for (size_t b = 0; b < s_mb_block_count; ++b) {
                const edgehmi_mb_block_t *blk = &s_mb_blocks[b];
                size_t len = edgehmi_mb_build_read_req(
                    blk->slave_id, blk->func_code, blk->start_addr, blk->quantity, tx_buf, sizeof(tx_buf));
                bool block_ok = false;
                for (int attempt = 0; attempt < 3; ++attempt) {
                    esp_task_wdt_reset(); /* Prevent TWDT panic when bus is offline across many blocks */
                    if (attempt > 0) s_mb_stats.retry_count++;
                    uint64_t t0 = now_ms();
                    s_mb_stats.tx_count++;
                    uart_flush_input(s_active_cfg.mb_port);
                    uart_write_bytes(s_active_cfg.mb_port, (const char *)tx_buf, len);
                    int rlen = uart_read_bytes(s_active_cfg.mb_port, blk_rx, sizeof(blk_rx),
                                               pdMS_TO_TICKS(s_active_cfg.mb_timeout_ms));
                    if (rlen <= 0) {
                        s_mb_stats.timeout_err++;
                        continue;
                    }
                    edgehmi_err_t prc = edgehmi_mb_parse_block_resp(
                        blk, s_active_cfg.points, blk_rx, (size_t)rlen, &s_bus, now_ms());
                    if (prc == EDGEHMI_OK) {
                        s_mb_stats.rx_ok++;
                        s_mb_stats.last_rtt_ms = (uint32_t)(now_ms() - t0);
                        block_ok = true;
                        break;
                    } else if (prc == EDGEHMI_ERR_CRC) {
                        s_mb_stats.crc_err++;
                    }
                }
                /* Explicitly mark points BAD after exhausting retries (fixes boot-time unplugged RS485) */
                if (!block_ok) {
                    uint64_t fail_ts = now_ms();
                    for (size_t k = 0; k < blk->point_count; ++k) {
                        const edgehmi_mb_point_t *pt = &s_active_cfg.points[blk->point_indices[k]];
                        edgehmi_bus_set_quality(&s_bus, pt->tag_idx, TAG_QUAL_BAD, fail_ts);
                    }
                }
            }
        }

        /* 2b. Optional Modbus TCP scan, native CAN (TWAI) RX drain & native GPIO/ADC Local IO */
        uint64_t ts = now_ms();
        if (s_active_cfg.modbus_tcp.enabled) {
            edgehmi_mbtcp_scan_blocks(&s_active_cfg, s_mb_blocks, s_mb_block_count, &s_bus, &s_mb_stats, ts);
        }
        edgehmi_can_poll_rx(&s_active_cfg, &s_bus, ts);
        edgehmi_local_io_sync(&s_active_cfg, &s_bus, ts);

        /* 3. Evaluate derived virtual calc_tags, stale check, interlock rules & persistent history */
        edgehmi_calc_eval(s_active_cfg.calc_tags, s_active_cfg.calc_tag_count, &s_bus, ts);
        edgehmi_bus_check_stale(&s_bus, ts, interval_ms * 10ULL);
        edgehmi_rules_eval(&s_rules, &s_bus, ts);

        static uint64_t s_last_hist_ms = 0;
        static uint64_t s_last_flush_ms = 0;
        if (ts - s_last_hist_ms >= 5000ULL) {
            edgehmi_history_record_sample(&s_history, &s_bus, ts);
            s_last_hist_ms = ts;
        }
        if (ts - s_last_flush_ms >= 60000ULL) {
            edgehmi_history_save_file(&s_history, "/cfg/history.bin");
            s_last_flush_ms = ts;
        }
        xSemaphoreGive(s_cfg_mutex);

        /* Mark both NVS config boot & ESP-IDF OTA app healthy after 15 seconds */
        static bool s_boot_ok_marked = false;
        if (!s_boot_ok_marked && ts >= 15000ULL) {
            edgehmi_boot_mark_healthy();
            edgehmi_ota_mark_app_valid();
            s_boot_ok_marked = true;
        }

        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(interval_ms));
    }
}

/* Core 1: LVGL 9 Render, Deferred UI Hot-Reload & Version-Tracked Sync Task */
static void ui_task(void *arg) {
    (void)arg;
    esp_task_wdt_add(NULL);
    for (;;) {
        xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
        if (s_ui_reload_pending) {
            s_ui_reload_pending = false;
            edgehmi_ui_build(&s_active_cfg, &s_bus, &s_history);
        }
        edgehmi_ui_sync_tick();
        lv_timer_handler();
        xSemaphoreGive(s_cfg_mutex);
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(16)); /* ~60Hz UI tick */
    }
}

static edgehmi_err_t reload_config_atomic(const char *json_buf, char *err_buf, size_t err_len) {
    edgehmi_config_t *candidate = (edgehmi_config_t *)edgehmi_alloc_buf(sizeof(edgehmi_config_t));
    edgehmi_bus_t *cand_bus = (edgehmi_bus_t *)edgehmi_alloc_buf(sizeof(edgehmi_bus_t));
    if (!candidate || !cand_bus) {
        free(candidate);
        free(cand_bus);
        snprintf(err_buf, err_len, "out of memory during config staging");
        return EDGEHMI_ERR_IO;
    }
    memset(candidate, 0, sizeof(*candidate));
    memset(cand_bus, 0, sizeof(*cand_bus));

    edgehmi_bus_init(cand_bus, NULL, NULL, NULL);
    edgehmi_err_t rc = edgehmi_config_load_json(json_buf, candidate, cand_bus, err_buf, err_len);
    if (rc != EDGEHMI_OK) {
        free(candidate);
        free(cand_bus);
        return rc;
    }

    xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
    memcpy(&s_active_cfg, candidate, sizeof(s_active_cfg));
    edgehmi_bus_init(&s_bus, bus_lock_cb, bus_unlock_cb, s_bus_mutex);
    /* Re-register points onto live bus & restore retentive NVS setpoints */
    for (size_t i = 0; i < s_active_cfg.point_count; ++i) {
        edgehmi_mb_point_t *pt = &s_active_cfg.points[i];
        double init_val = pt->default_val;
        if (pt->retentive) {
            double saved = 0.0;
            if (nvs_retentive_load(pt->tag_name, &saved)) {
                init_val = saved;
            }
        }
        edgehmi_bus_register(&s_bus, pt->tag_name, pt->type, pt->access,
                             pt->unit, init_val, &pt->tag_idx);
    }
    /* Re-register native Local IO & CAN signals onto live bus */
    for (size_t i = 0; i < s_active_cfg.local_io_count; ++i) {
        edgehmi_local_io_t *io = &s_active_cfg.local_io[i];
        edgehmi_tag_type_t tt = (io->mode == LOCAL_IO_ADC) ? TAG_TYPE_F32
                                : (io->mode == LOCAL_IO_PWM) ? TAG_TYPE_U16
                                                             : TAG_TYPE_BOOL;
        edgehmi_access_t acc = (io->mode == LOCAL_IO_DO || io->mode == LOCAL_IO_PWM) ? TAG_ACCESS_RW : TAG_ACCESS_R;
        double init_val = io->default_val;
        if (io->retentive) {
            double saved = 0.0;
            if (nvs_retentive_load(io->tag_name, &saved)) init_val = saved;
        }
        edgehmi_bus_register(&s_bus, io->tag_name, tt, acc, io->unit, init_val, &io->tag_idx);
    }
    for (size_t i = 0; i < s_active_cfg.can_bus.signal_count; ++i) {
        edgehmi_can_signal_t *sig = &s_active_cfg.can_bus.signals[i];
        edgehmi_bus_register(&s_bus, sig->tag_name, TAG_TYPE_F32, TAG_ACCESS_R,
                             sig->unit, sig->default_val, &sig->tag_idx);
    }
    for (size_t i = 0; i < s_active_cfg.calc_tag_count; ++i) {
        edgehmi_calc_tag_t *ct = &s_active_cfg.calc_tags[i];
        edgehmi_bus_register(&s_bus, ct->tag_name, TAG_TYPE_F32, TAG_ACCESS_R,
                             ct->unit, 0.0, &ct->tag_idx);
    }
    edgehmi_calc_bind(s_active_cfg.calc_tags, s_active_cfg.calc_tag_count, &s_bus);

    s_mb_block_count = edgehmi_mb_plan_blocks(
        s_active_cfg.points, s_active_cfg.point_count,
        s_mb_blocks, EDGEHMI_MAX_MODBUS_BLOCKS, 4, 32);
    edgehmi_rules_init(&s_rules, s_active_cfg.rules, s_active_cfg.rule_count, on_alarm_event, NULL);
    edgehmi_history_init(&s_history, &s_bus);
    edgehmi_history_load_file(&s_history, "/cfg/history.bin");

    /* Ensure RS485 UART, Local IO, CAN (TWAI) & Modbus TCP are initialized / reconfigured */
    rs485_configure(&s_active_cfg);
    edgehmi_local_io_configure(&s_active_cfg);
    edgehmi_fieldbus_ext_configure(&s_active_cfg);

    /* Apply JSON watchdog_timeout_s to ESP-IDF Task Watchdog */
    if (s_active_cfg.watchdog_timeout_s >= 3) {
        esp_task_wdt_config_t twdt_cfg = {
            .timeout_ms = s_active_cfg.watchdog_timeout_s * 1000u,
            .idle_core_mask = 0,
            .trigger_panic = true,
        };
        esp_task_wdt_reconfigure(&twdt_cfg);
    }

    /* Defer LVGL screen rebuild to Core 1 ui_task to guarantee single-threaded LVGL safety */
    s_ui_reload_pending = true;
    xSemaphoreGive(s_cfg_mutex);

    edgehmi_cloud_mqtt_reconfigure();

    free(candidate);
    free(cand_bus);
    ESP_LOGI(TAG, "Hot-reloaded config v%s (%u points -> %u MB blocks, %u rules, %u pages)",
             s_active_cfg.version, (unsigned)s_active_cfg.point_count,
             (unsigned)s_mb_block_count, (unsigned)s_active_cfg.rule_count,
             (unsigned)s_active_cfg.page_count);
    return EDGEHMI_OK;
}

void app_main(void) {
    ESP_ERROR_CHECK(nvs_flash_init());
    s_bus_mutex = xSemaphoreCreateMutex();
    s_cfg_mutex = xSemaphoreCreateMutex();
    edgehmi_bus_init(&s_bus, bus_lock_cb, bus_unlock_cb, s_bus_mutex);

    edgehmi_bsp_init();

    if (edgehmi_storage_init() == EDGEHMI_OK) {
        edgehmi_boot_begin_check();

        /* Check for offline SD/USB .epk update package first (auto-reboots on firmware .epk) */
        edgehmi_err_t epk_rc = edgehmi_ota_apply_offline_epk("/sdcard/update.epk", reload_config_atomic);
        if (epk_rc != EDGEHMI_OK) {
            char *buf = (char *)edgehmi_alloc_buf(65536);
            size_t len = 0;
            if (buf && edgehmi_storage_read_active(buf, 65536, &len) == EDGEHMI_OK) {
                char err[128] = {0};
                if (reload_config_atomic(buf, err, sizeof(err)) != EDGEHMI_OK) {
                    ESP_LOGE(TAG, "Boot config invalid: %s", err);
                }
            }
            free(buf);
        }
    }

    /* Core 0: Wi-Fi/SoftAP/4G + HTTP/OTA/Historian + Cloud MQTT + LAN discovery */
    edgehmi_net_start(&s_active_cfg);
    edgehmi_http_start(&s_bus, &s_active_cfg, &s_mb_stats, &s_history, s_cfg_mutex, reload_config_atomic);
    edgehmi_discovery_start(&s_active_cfg, &s_bus, s_cfg_mutex, 80);
    edgehmi_cloud_mqtt_start(&s_active_cfg, &s_bus, s_cfg_mutex);

    /* Core 1: Deterministic fieldbus + interlock + LVGL rendering */
    xTaskCreatePinnedToCore(control_task, "edgehmi_ctrl", 6144, NULL, 6, NULL, 1);
    xTaskCreatePinnedToCore(ui_task,      "edgehmi_ui",   8192, NULL, 5, NULL, 1);
}
