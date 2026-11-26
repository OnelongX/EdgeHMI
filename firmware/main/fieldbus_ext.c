#include "fieldbus_ext.h"

#include "can_codec.h"
#include "driver/twai.h"
#include "esp_log.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "modbus_rtu.h"
#include "modbus_tcp.h"

#include <string.h>
#include <sys/time.h>

static const char *TAG = "edgehmi_bus_ext";
static bool s_can_running = false;
static int s_tcp_sock = -1;
static uint16_t s_tcp_tid = 1;

edgehmi_err_t edgehmi_fieldbus_ext_configure(const edgehmi_config_t *cfg) {
    if (!cfg) return EDGEHMI_ERR_INVALID_ARG;

    if (s_tcp_sock >= 0) {
        close(s_tcp_sock);
        s_tcp_sock = -1;
    }

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
    return EDGEHMI_OK;
}

size_t edgehmi_can_poll_rx(const edgehmi_config_t *cfg, edgehmi_bus_t *bus, uint64_t now_ms) {
    if (!s_can_running || !cfg || !bus) {
        return 0;
    }
    size_t updated = 0;
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

    /* Flush pending writes via Modbus TCP */
    for (size_t i = 0; i < cfg->point_count; ++i) {
        const edgehmi_mb_point_t *pt = &cfg->points[i];
        double wval = 0.0;
        if (edgehmi_bus_pop_write(bus, pt->tag_idx, &wval)) {
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
