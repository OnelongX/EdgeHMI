#include "fieldbus_ext.h"

#include "can_codec.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "modbus_rtu.h"
#include "modbus_tcp.h"
#include "nvs.h"

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
#include "esp_twai.h"
#include "esp_twai_onchip.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#else
#include "driver/twai.h"
#endif

#include <string.h>
#include <sys/time.h>

static const char *TAG = "edgehmi_bus_ext";
static bool s_can_running = false;
static int s_tcp_sock = -1;
static uint16_t s_tcp_tid = 1;

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
typedef struct {
    uint32_t id;
    uint8_t extd;
    uint8_t dlc;
    uint8_t data[8];
} can_rx_item_t;

static twai_node_handle_t s_twai_node = NULL;
static QueueHandle_t s_can_rx_q = NULL;

static bool IRAM_ATTR on_twai_rx_done(twai_node_handle_t handle,
                                      const twai_rx_done_event_data_t *edata,
                                      void *user_ctx) {
    (void)edata;
    (void)user_ctx;
    uint8_t buf[8] = {0};
    twai_frame_t frame = {
        .buffer = buf,
        .buffer_len = sizeof(buf),
    };
    if (twai_node_receive_from_isr(handle, &frame) == ESP_OK &&
        !frame.header.rtr && frame.header.dlc <= 8 && s_can_rx_q) {
        can_rx_item_t item;
        item.id = frame.header.id;
        item.extd = frame.header.ide ? 1u : 0u;
        item.dlc = (uint8_t)frame.header.dlc;
        memcpy(item.data, buf, item.dlc);
        BaseType_t woken = pdFALSE;
        xQueueSendFromISR(s_can_rx_q, &item, &woken);
        return woken == pdTRUE;
    }
    return false;
}
#endif

edgehmi_err_t edgehmi_fieldbus_ext_configure(const edgehmi_config_t *cfg) {
    if (!cfg) return EDGEHMI_ERR_INVALID_ARG;

    if (s_tcp_sock >= 0) {
        close(s_tcp_sock);
        s_tcp_sock = -1;
    }

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
    if (s_twai_node) {
        twai_node_disable(s_twai_node);
        twai_node_delete(s_twai_node);
        s_twai_node = NULL;
        s_can_running = false;
    }
    if (cfg->can_bus.enabled && cfg->can_bus.tx_pin >= 0 && cfg->can_bus.rx_pin >= 0) {
        if (!s_can_rx_q) {
            s_can_rx_q = xQueueCreate(32, sizeof(can_rx_item_t));
        }
        twai_onchip_node_config_t ncfg = {
            .io_cfg = {
                .tx = (gpio_num_t)cfg->can_bus.tx_pin,
                .rx = (gpio_num_t)cfg->can_bus.rx_pin,
                .quanta_clk_out = GPIO_NUM_NC,
                .bus_off_indicator = GPIO_NUM_NC,
            },
            .bit_timing = {
                .bitrate = cfg->can_bus.bitrate >= 25000 ? cfg->can_bus.bitrate : 500000,
            },
            .fail_retry_cnt = -1,
            .tx_queue_depth = 8,
        };
        twai_event_callbacks_t cbs = {
            .on_rx_done = on_twai_rx_done,
        };
        if (s_can_rx_q &&
            twai_new_node_onchip(&ncfg, &s_twai_node) == ESP_OK &&
            twai_node_register_event_callbacks(s_twai_node, &cbs, NULL) == ESP_OK &&
            twai_node_enable(s_twai_node) == ESP_OK) {
            s_can_running = true;
            ESP_LOGI(TAG, "TWAI v2 CAN 2.0B started (%lu bps, TX=%d RX=%d, %u signals)",
                     (unsigned long)ncfg.bit_timing.bitrate,
                     (int)cfg->can_bus.tx_pin, (int)cfg->can_bus.rx_pin,
                     (unsigned)cfg->can_bus.signal_count);
        } else {
            if (s_twai_node) {
                twai_node_delete(s_twai_node);
                s_twai_node = NULL;
            }
            ESP_LOGE(TAG, "TWAI v2 CAN driver start failed");
        }
    }
#else
    if (s_can_running) {
        twai_stop();
        twai_driver_uninstall();
        s_can_running = false;
    }

    if (cfg->can_bus.enabled && cfg->can_bus.tx_pin >= 0 && cfg->can_bus.rx_pin >= 0) {
        twai_general_config_t g_cfg = TWAI_GENERAL_CONFIG_DEFAULT(
            (gpio_num_t)cfg->can_bus.tx_pin, (gpio_num_t)cfg->can_bus.rx_pin, TWAI_MODE_NORMAL);
        twai_timing_config_t t_cfg = TWAI_TIMING_CONFIG_500KBITS();
        if (cfg->can_bus.bitrate == 250000) {
            t_cfg = (twai_timing_config_t)TWAI_TIMING_CONFIG_250KBITS();
        } else if (cfg->can_bus.bitrate == 125000) {
            t_cfg = (twai_timing_config_t)TWAI_TIMING_CONFIG_125KBITS();
        }
        twai_filter_config_t f_cfg = TWAI_FILTER_CONFIG_ACCEPT_ALL();

        if (twai_driver_install(&g_cfg, &t_cfg, &f_cfg) == ESP_OK && twai_start() == ESP_OK) {
            s_can_running = true;
            ESP_LOGI(TAG, "TWAI CAN 2.0B started (%lu bps, TX=%d RX=%d, %u signals)",
                     (unsigned long)cfg->can_bus.bitrate,
                     (int)cfg->can_bus.tx_pin, (int)cfg->can_bus.rx_pin,
                     (unsigned)cfg->can_bus.signal_count);
        } else {
            ESP_LOGE(TAG, "TWAI CAN driver start failed");
        }
    }
#endif
    return EDGEHMI_OK;
}

size_t edgehmi_can_poll_rx(const edgehmi_config_t *cfg, edgehmi_bus_t *bus, uint64_t now_ms) {
    if (!s_can_running || !cfg || !bus) {
        return 0;
    }
    size_t updated = 0;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
    if (!s_can_rx_q) return 0;
    can_rx_item_t item;
    for (int i = 0; i < 16; ++i) {
        if (xQueueReceive(s_can_rx_q, &item, 0) != pdTRUE) {
            break;
        }
        updated += edgehmi_can_ingest_frame(
            &cfg->can_bus, item.id, item.extd != 0, item.data, item.dlc, bus, now_ms);
    }
#else
    twai_message_t msg;
    for (int i = 0; i < 16; ++i) {
        if (twai_receive(&msg, 0) != ESP_OK) {
            break;
        }
        if (msg.rtr || msg.data_length_code > 8) {
            continue;
        }
        updated += edgehmi_can_ingest_frame(
            &cfg->can_bus, msg.identifier, msg.extd != 0,
            msg.data, msg.data_length_code, bus, now_ms);
    }
#endif
    return updated;
}

static bool ensure_tcp_connected(const edgehmi_mb_tcp_cfg_t *tcfg) {
    if (s_tcp_sock >= 0) return true;
    if (!tcfg->enabled || tcfg->host[0] == '\0') return false;

    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (sock < 0) return false;

    struct timeval tv;
    tv.tv_sec = (long)(tcfg->timeout_ms / 1000u);
    tv.tv_usec = (long)((tcfg->timeout_ms % 1000u) * 1000u);
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(tcfg->port ? tcfg->port : 502);
    if (inet_pton(AF_INET, tcfg->host, &dst.sin_addr) != 1 ||
        connect(sock, (struct sockaddr *)&dst, sizeof(dst)) != 0) {
        close(sock);
        return false;
    }
    s_tcp_sock = sock;
    ESP_LOGI(TAG, "Modbus TCP connected to %s:%u", tcfg->host, (unsigned)tcfg->port);
    return true;
}

void edgehmi_mbtcp_scan_blocks(const edgehmi_config_t *cfg,
                               const edgehmi_mb_block_t *blocks,
                               size_t block_count,
                               edgehmi_bus_t *bus,
                               edgehmi_mb_stats_t *stats,
                               uint64_t now_ms) {
    if (!cfg || !cfg->modbus_tcp.enabled || !blocks || !bus) {
        return;
    }
    if (!ensure_tcp_connected(&cfg->modbus_tcp)) {
        for (size_t b = 0; b < block_count; ++b) {
            for (size_t k = 0; k < blocks[b].point_count; ++k) {
                edgehmi_bus_set_quality(bus, cfg->points[blocks[b].point_indices[k]].tag_idx,
                                        TAG_QUAL_BAD, now_ms);
            }
        }
        return;
    }

    uint8_t tx[20];
    uint8_t rx[260];

    /* Flush pending writes via Modbus TCP (+ persist retentive NVS) */
    for (size_t i = 0; i < cfg->point_count; ++i) {
        const edgehmi_mb_point_t *pt = &cfg->points[i];
        double wval = 0.0;
        if (edgehmi_bus_pop_write(bus, pt->tag_idx, &wval)) {
            if (pt->retentive) {
                nvs_handle_t nh;
                if (nvs_open("edgehmi_ret", NVS_READWRITE, &nh) == ESP_OK) {
                    char key[16];
                    edgehmi_nvs_safe_key(pt->tag_name, key);
                    nvs_set_blob(nh, key, &wval, sizeof(wval));
                    nvs_commit(nh);
                    nvs_close(nh);
                }
            }
            uint16_t tid = s_tcp_tid++;
            size_t len = 0;
            if (pt->type == TAG_TYPE_U32 || pt->type == TAG_TYPE_F32) {
                len = edgehmi_mbtcp_build_write_multi(tid, pt, wval, tx, sizeof(tx));
            } else {
                uint8_t wfc = (pt->func_code == 1 || pt->func_code == 5) ? 5 : 6;
                uint16_t raw = edgehmi_mb_eng_to_raw16(pt, wval);
                len = edgehmi_mbtcp_build_write_single(tid, pt->slave_id, wfc, pt->reg_addr, raw, tx, sizeof(tx));
            }
            if (len > 0) {
                if (stats) stats->tx_count++;
                if (send(s_tcp_sock, tx, len, 0) < 0 || recv(s_tcp_sock, rx, sizeof(rx), 0) <= 0) {
                    close(s_tcp_sock);
                    s_tcp_sock = -1;
                    return;
                }
            }
        }
    }

    /* Block read scan */
    for (size_t b = 0; b < block_count; ++b) {
        const edgehmi_mb_block_t *blk = &blocks[b];
        uint16_t tid = s_tcp_tid++;
        size_t len = edgehmi_mbtcp_build_read_req(
            tid, blk->slave_id, blk->func_code, blk->start_addr, blk->quantity, tx, sizeof(tx));
        if (stats) stats->tx_count++;
        if (send(s_tcp_sock, tx, len, 0) < 0) {
            close(s_tcp_sock);
            s_tcp_sock = -1;
            return;
        }
        int rlen = recv(s_tcp_sock, rx, sizeof(rx), 0);
        if (rlen <= 0) {
            if (stats) stats->timeout_err++;
            close(s_tcp_sock);
            s_tcp_sock = -1;
            for (size_t k = 0; k < blk->point_count; ++k) {
                edgehmi_bus_set_quality(bus, cfg->points[blk->point_indices[k]].tag_idx,
                                        TAG_QUAL_BAD, now_ms);
            }
            return;
        }
        if (edgehmi_mbtcp_parse_block_resp(tid, blk, cfg->points, rx, (size_t)rlen, bus, now_ms) == EDGEHMI_OK) {
            if (stats) stats->rx_ok++;
        }
    }
}

/* Core 0 Northbound Modbus TCP Server Gateway — exposes unified data_bus to SCADA / PLC */
static const edgehmi_config_t *s_gw_cfg = NULL;
static edgehmi_bus_t *s_gw_bus = NULL;
static edgehmi_history_t *s_gw_hist = NULL;
static SemaphoreHandle_t s_gw_mutex = NULL;

static void mbtcp_server_task(void *arg) {
    (void)arg;
    int listen_fd = -1;
    uint16_t bound_port = 0;
    uint8_t rx_buf[260];
    uint8_t tx_buf[260];

    for (;;) {
        bool enabled = false;
        uint16_t port = 502;
        if (s_gw_mutex) xSemaphoreTake(s_gw_mutex, portMAX_DELAY);
        if (s_gw_cfg) {
            enabled = s_gw_cfg->modbus_tcp.server_enabled;
            port = s_gw_cfg->modbus_tcp.server_port ? s_gw_cfg->modbus_tcp.server_port : 502;
        }
        if (s_gw_mutex) xSemaphoreGive(s_gw_mutex);

        if (!enabled) {
            if (listen_fd >= 0) {
                close(listen_fd);
                listen_fd = -1;
                bound_port = 0;
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (listen_fd >= 0 && bound_port != port) {
            close(listen_fd);
            listen_fd = -1;
        }
        if (listen_fd < 0) {
            listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
            if (listen_fd < 0) {
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }
            int opt = 1;
            setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
            struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
            setsockopt(listen_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

            struct sockaddr_in saddr;
            memset(&saddr, 0, sizeof(saddr));
            saddr.sin_family = AF_INET;
            saddr.sin_port = htons(port);
            saddr.sin_addr.s_addr = htonl(INADDR_ANY);
            if (bind(listen_fd, (struct sockaddr *)&saddr, sizeof(saddr)) != 0 ||
                listen(listen_fd, 2) != 0) {
                close(listen_fd);
                listen_fd = -1;
                vTaskDelay(pdMS_TO_TICKS(3000));
                continue;
            }
            bound_port = port;
            ESP_LOGI(TAG, "Modbus TCP Server Gateway listening on port %u", (unsigned)port);
        }

        struct sockaddr_in caddr;
        socklen_t clen = sizeof(caddr);
        int cli = accept(listen_fd, (struct sockaddr *)&caddr, &clen);
        if (cli < 0) {
            continue;
        }
        struct timeval ctv = { .tv_sec = 2, .tv_usec = 0 };
        setsockopt(cli, SOL_SOCKET, SO_RCVTIMEO, &ctv, sizeof(ctv));

        for (;;) {
            /* Re-verify gateway is still enabled on same port across hot-reloads */
            bool still_ok = false;
            if (s_gw_mutex) xSemaphoreTake(s_gw_mutex, portMAX_DELAY);
            if (s_gw_cfg) {
                uint16_t cfg_port = s_gw_cfg->modbus_tcp.server_port ? s_gw_cfg->modbus_tcp.server_port : 502;
                still_ok = s_gw_cfg->modbus_tcp.server_enabled && (cfg_port == bound_port);
            }
            if (s_gw_mutex) xSemaphoreGive(s_gw_mutex);
            if (!still_ok) {
                break;
            }

            int n = recv(cli, rx_buf, sizeof(rx_buf), 0);
            if (n <= 0) {
                break;
            }
            size_t rlen = 0;
            uint64_t ts = (uint64_t)(esp_timer_get_time() / 1000LL);
            if (s_gw_mutex) xSemaphoreTake(s_gw_mutex, portMAX_DELAY);
            if (s_gw_bus) {
                uint8_t fc = (n >= 8) ? rx_buf[7] : 0;
                int16_t audit_idx = -1;
                double old_v = 0.0;
                if (n >= 12 && (fc == 5 || fc == 6 || fc == 16)) {
                    uint16_t addr = (uint16_t)(((uint16_t)rx_buf[8] << 8) | rx_buf[9]);
                    if (fc == 5 && addr < s_gw_bus->count) audit_idx = (int16_t)addr;
                    else if (fc == 6 && addr >= 1000u && (addr - 1000u) < s_gw_bus->count) audit_idx = (int16_t)(addr - 1000u);
                    else if (fc == 16 && (addr % 2u) == 0u && (addr / 2u) < s_gw_bus->count) audit_idx = (int16_t)(addr / 2u);
                    if (audit_idx >= 0) edgehmi_bus_get(s_gw_bus, audit_idx, &old_v, NULL);
                }
                rlen = edgehmi_mbtcp_handle_server_req(rx_buf, (size_t)n, s_gw_bus, ts, tx_buf, sizeof(tx_buf));
                if (s_gw_hist && audit_idx >= 0 && rlen >= 12 && (tx_buf[7] & 0x80u) == 0) {
                    edgehmi_history_record_audit(
                        s_gw_hist, AUDIT_SRC_MODBUS_GW, "scada_tcp",
                        s_gw_bus->tags[audit_idx].name, old_v,
                        s_gw_bus->tags[audit_idx].pending_write, ts);
                }
            }
            if (s_gw_mutex) xSemaphoreGive(s_gw_mutex);
            if (rlen > 0 && send(cli, tx_buf, rlen, 0) < 0) {
                break;
            }
        }
        close(cli);
    }
}

edgehmi_err_t edgehmi_mbtcp_server_start(const edgehmi_config_t *active_cfg,
                                         edgehmi_bus_t *bus,
                                         edgehmi_history_t *history,
                                         SemaphoreHandle_t cfg_mutex) {
    s_gw_cfg = active_cfg;
    s_gw_bus = bus;
    s_gw_hist = history;
    s_gw_mutex = cfg_mutex;
    xTaskCreatePinnedToCore(mbtcp_server_task, "edgehmi_mbgw", 4096, NULL, 3, NULL, 0);
    return EDGEHMI_OK;
}
