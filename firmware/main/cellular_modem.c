#include "cellular_modem.h"

#include "data_bus.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif_ppp.h"
#include "freertos/task.h"
#include "nvs.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "edgehmi_cell";

typedef struct {
    esp_netif_driver_base_t base;
    uart_port_t port;
} ppp_uart_driver_t;

static const edgehmi_config_t *s_cfg = NULL;
static SemaphoreHandle_t s_cfg_mutex = NULL;
static SemaphoreHandle_t s_stat_mutex = NULL;
static esp_netif_t *s_wifi_netif = NULL;
static esp_netif_t *s_ppp_netif = NULL;
static ppp_uart_driver_t s_ppp_drv;
static edgehmi_cell_status_t s_status;
static edgehmi_wan_failover_t s_failover;
static volatile bool s_wifi_has_ip = false;
static volatile bool s_ppp_has_ip = false;
static volatile bool s_ppp_data_mode = false;

static esp_err_t ppp_uart_transmit(void *h, void *buffer, size_t len) {
    ppp_uart_driver_t *drv = (ppp_uart_driver_t *)h;
    if (!drv || !buffer || len == 0 || !s_ppp_data_mode) {
        return ESP_OK;
    }
    int written = uart_write_bytes(drv->port, (const char *)buffer, len);
    return (written >= 0) ? ESP_OK : ESP_FAIL;
}

static esp_err_t ppp_post_attach(esp_netif_t *esp_netif, void *args) {
    ppp_uart_driver_t *drv = (ppp_uart_driver_t *)args;
    drv->base.netif = esp_netif;
    esp_netif_driver_ifconfig_t drv_cfg = {
        .handle = drv,
        .transmit = ppp_uart_transmit,
        .driver_free_rx_buffer = NULL,
    };
    return esp_netif_set_driver_config(esp_netif, &drv_cfg);
}

static void on_ppp_ip_event(void *arg, esp_event_base_t base, int32_t event_id, void *data) {
    (void)arg;
    (void)base;
    (void)data;
    if (event_id == IP_EVENT_PPP_GOT_IP) {
        s_ppp_has_ip = true;
        if (s_stat_mutex) {
            xSemaphoreTake(s_stat_mutex, portMAX_DELAY);
            s_status.ppp_up = true;
            xSemaphoreGive(s_stat_mutex);
        }
        ESP_LOGI(TAG, "4G/5G PPPoS link UP (IP acquired)");
    } else if (event_id == IP_EVENT_PPP_LOST_IP) {
        s_ppp_has_ip = false;
        s_ppp_data_mode = false;
        if (s_stat_mutex) {
            xSemaphoreTake(s_stat_mutex, portMAX_DELAY);
            s_status.ppp_up = false;
            xSemaphoreGive(s_stat_mutex);
        }
        ESP_LOGW(TAG, "4G/5G PPPoS link LOST");
    }
}

static void modem_hw_power_cycle(const edgehmi_cellular_cfg_t *c) {
    if (c->reset_pin >= 0 && c->reset_pin <= 48) {
        gpio_set_direction((gpio_num_t)c->reset_pin, GPIO_MODE_OUTPUT);
        gpio_set_level((gpio_num_t)c->reset_pin, 1);
        vTaskDelay(pdMS_TO_TICKS(350));
        gpio_set_level((gpio_num_t)c->reset_pin, 0);
        vTaskDelay(pdMS_TO_TICKS(3000));
    } else if (c->pwrkey_pin >= 0 && c->pwrkey_pin <= 48) {
        gpio_set_direction((gpio_num_t)c->pwrkey_pin, GPIO_MODE_OUTPUT);
        gpio_set_level((gpio_num_t)c->pwrkey_pin, 1);
        vTaskDelay(pdMS_TO_TICKS(1200));
        gpio_set_level((gpio_num_t)c->pwrkey_pin, 0);
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
    if (s_stat_mutex) {
        xSemaphoreTake(s_stat_mutex, portMAX_DELAY);
        s_status.hw_reset_count++;
        xSemaphoreGive(s_stat_mutex);
    }
}

static bool send_at_cmd(uart_port_t port, const char *cmd, char *resp, size_t cap, uint32_t timeout_ms) {
    uart_flush_input(port);
    uart_write_bytes(port, cmd, strlen(cmd));
    size_t pos = 0;
    resp[0] = '\0';
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (xTaskGetTickCount() < deadline && pos + 1 < cap) {
        int r = uart_read_bytes(port, (uint8_t *)resp + pos, cap - 1 - pos, pdMS_TO_TICKS(80));
        if (r > 0) {
            pos += (size_t)r;
            resp[pos] = '\0';
            if (strstr(resp, "OK") || strstr(resp, "CONNECT") || strstr(resp, "ERROR")) {
                return strstr(resp, "ERROR") == NULL;
            }
        }
    }
    return pos > 0 && strstr(resp, "OK") != NULL;
}

static void update_wan_route(void) {
    edgehmi_wan_link_t prev = s_failover.active_link;
    edgehmi_wan_link_t cur = edgehmi_wan_failover_tick(&s_failover, s_wifi_has_ip, s_ppp_has_ip);
    if (cur != prev) {
        if (cur == WAN_LINK_WIFI && s_wifi_netif) {
            esp_netif_set_default_netif(s_wifi_netif);
            ESP_LOGI(TAG, "WAN active route switched -> Wi-Fi");
        } else if (cur == WAN_LINK_CELLULAR && s_ppp_netif) {
            esp_netif_set_default_netif(s_ppp_netif);
            ESP_LOGW(TAG, "WAN active route switched -> 4G/5G Cellular (%s)",
                     edgehmi_cell_rat_str(s_status.rat));
        }
    }
}

static void cellular_supervisor_task(void *arg) {
    (void)arg;
    char resp[256];
    uint8_t ppp_rx[512];
    bool uart_ready = false;
    uart_port_t active_port = UART_NUM_2;

    for (;;) {
        edgehmi_cellular_cfg_t ccfg;
        edgehmi_net_policy_t pol = NET_POLICY_WIFI_PRIMARY;
        memset(&ccfg, 0, sizeof(ccfg));
        if (s_cfg_mutex) xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
        if (s_cfg) {
            ccfg = s_cfg->net.cellular;
            pol = s_cfg->net.failover_policy;
        }
        if (s_cfg_mutex) xSemaphoreGive(s_cfg_mutex);
        s_failover.policy = pol;

        /* Allow NVS APN override set via /setup mobile portal */
        nvs_handle_t nh;
        if (nvs_open("edgehmi_cell", NVS_READONLY, &nh) == ESP_OK) {
            size_t al = sizeof(ccfg.apn);
            nvs_get_str(nh, "apn", ccfg.apn, &al);
            nvs_close(nh);
        }

        update_wan_route();

        if (!ccfg.enabled || ccfg.tx_pin < 0 || ccfg.rx_pin < 0) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        if (ccfg.iface == CELL_IFACE_USB_ECM) {
            /* High-throughput USB OTG CDC-ECM/NCM mode (5G NR / 4G Cat.4) */
            ESP_LOGD(TAG, "USB OTG CDC-ECM/NCM cellular interface monitored");
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        if (!uart_ready) {
            active_port = (uart_port_t)(ccfg.uart_port <= 2 ? ccfg.uart_port : 2);
            uart_config_t uc = {
                .baud_rate = (int)(ccfg.baudrate >= 9600 ? ccfg.baudrate : 115200),
                .data_bits = UART_DATA_8_BITS,
                .parity = UART_PARITY_DISABLE,
                .stop_bits = UART_STOP_BITS_1,
                .flow_ctrl = (ccfg.rts_pin >= 0 && ccfg.cts_pin >= 0)
                                 ? UART_HW_FLOWCTRL_CTS_RTS
                                 : UART_HW_FLOWCTRL_DISABLE,
                .source_clk = UART_SCLK_DEFAULT,
            };
            if (uart_driver_install(active_port, 2048, 2048, 0, NULL, 0) == ESP_OK) {
                uart_param_config(active_port, &uc);
                uart_set_pin(active_port, ccfg.tx_pin, ccfg.rx_pin,
                             ccfg.rts_pin >= 0 ? ccfg.rts_pin : UART_PIN_NO_CHANGE,
                             ccfg.cts_pin >= 0 ? ccfg.cts_pin : UART_PIN_NO_CHANGE);
                s_ppp_drv.port = active_port;
                s_ppp_drv.base.post_attach = ppp_post_attach;
                esp_netif_attach(s_ppp_netif, &s_ppp_drv);
                uart_ready = true;
                ESP_LOGI(TAG, "Cellular UART%d ready (%lu bps, gen=%d, APN=%s)",
                         (int)active_port, (unsigned long)ccfg.baudrate,
                         (int)ccfg.generation, ccfg.apn);
            } else {
                vTaskDelay(pdMS_TO_TICKS(5000));
                continue;
            }
        }

        if (!s_ppp_data_mode) {
            /* 1. Sync AT command channel (hardware power-cycle modem if unresponsive) */
            if (!send_at_cmd(active_port, "AT\r\n", resp, sizeof(resp), 800)) {
                ESP_LOGW(TAG, "4G/5G modem unresponsive — pulsing PWRKEY/RESET");
                modem_hw_power_cycle(&ccfg);
                continue;
            }
            send_at_cmd(active_port, "ATE0\r\n", resp, sizeof(resp), 500);

            /* 2. SIM & 4G/5G Radio Diagnostics */
            bool sim_ok = send_at_cmd(active_port, "AT+CPIN?\r\n", resp, sizeof(resp), 800) &&
                          strstr(resp, "READY") != NULL;
            int8_t dbm = 0;
            uint8_t bars = 0;
            if (send_at_cmd(active_port, "AT+CSQ\r\n", resp, sizeof(resp), 800)) {
                edgehmi_cell_parse_csq(resp, &dbm, &bars);
            }
            char cmd[96];
            if (edgehmi_cell_build_rat_cmd(ccfg.generation, cmd, sizeof(cmd)) > 0) {
                send_at_cmd(active_port, cmd, resp, sizeof(resp), 800);
            }
            edgehmi_cell_rat_t rat = CELL_RAT_4G_LTE;
            char oper[32] = {0};
            if (send_at_cmd(active_port, "AT+QNWINFO\r\n", resp, sizeof(resp), 800) ||
                send_at_cmd(active_port, "AT+COPS?\r\n", resp, sizeof(resp), 800)) {
                edgehmi_cell_parse_nwinfo(resp, &rat, oper, sizeof(oper));
            }
            char iccid[24] = {0};
            if (send_at_cmd(active_port, "AT+QCCID\r\n", resp, sizeof(resp), 800) ||
                send_at_cmd(active_port, "AT+CCID\r\n", resp, sizeof(resp), 800)) {
                edgehmi_cell_parse_iccid(resp, iccid, sizeof(iccid));
            }

            /* Check 5G NR (+C5GREG?) first, then 4G LTE (+CEREG?) */
            bool reg = false;
            if (send_at_cmd(active_port, "AT+C5GREG?\r\n", resp, sizeof(resp), 800)) {
                edgehmi_cell_parse_reg(resp, &reg);
            }
            if (!reg && send_at_cmd(active_port, "AT+CEREG?\r\n", resp, sizeof(resp), 800)) {
                edgehmi_cell_parse_reg(resp, &reg);
            }

            if (s_stat_mutex) {
                xSemaphoreTake(s_stat_mutex, portMAX_DELAY);
                s_status.sim_ready = sim_ok;
                s_status.registered = reg;
                s_status.rssi_dbm = dbm;
                s_status.signal_bars = bars;
                s_status.rat = rat;
                if (oper[0]) edgehmi_copy_str(s_status.operator_name, sizeof(s_status.operator_name), oper);
                if (iccid[0]) edgehmi_copy_str(s_status.iccid, sizeof(s_status.iccid), iccid);
                xSemaphoreGive(s_stat_mutex);
            }

            if (!sim_ok || !reg) {
                vTaskDelay(pdMS_TO_TICKS(3000));
                continue;
            }

            /* 3. Configure PDP APN & Dial PPPoS (ATD*99#) */
            if (edgehmi_cell_build_apn_cmd(ccfg.apn, cmd, sizeof(cmd)) > 0) {
                send_at_cmd(active_port, cmd, resp, sizeof(resp), 1000);
            }
            if (ccfg.apn_user[0] != '\0') {
                esp_netif_ppp_set_auth(s_ppp_netif, NETIF_PPP_AUTHTYPE_PAP, ccfg.apn_user, ccfg.apn_pass);
            }
            if (send_at_cmd(active_port, "ATD*99#\r\n", resp, sizeof(resp), 5000) &&
                strstr(resp, "CONNECT") != NULL) {
                s_ppp_data_mode = true;
                esp_netif_action_start(s_ppp_netif, NULL, 0, NULL);
                if (s_stat_mutex) {
                    xSemaphoreTake(s_stat_mutex, portMAX_DELAY);
                    s_status.reconnect_count++;
                    xSemaphoreGive(s_stat_mutex);
                }
                ESP_LOGI(TAG, "Modem entered PPPoS data mode (%s, %d dBm)",
                         edgehmi_cell_rat_str(rat), (int)dbm);
            } else {
                vTaskDelay(pdMS_TO_TICKS(4000));
            }
        } else {
            /* Pump PPP frames from UART into lwIP esp_netif */
            int r = uart_read_bytes(active_port, ppp_rx, sizeof(ppp_rx), pdMS_TO_TICKS(200));
            if (r > 0) {
                esp_netif_receive(s_ppp_netif, ppp_rx, (size_t)r, NULL);
            }
            static uint32_t s_tick = 0;
            if ((++s_tick % 25u) == 0u) {
                update_wan_route();
            }
        }
    }
}

edgehmi_err_t edgehmi_cellular_start(const edgehmi_config_t *active_cfg,
                                     esp_netif_t *wifi_sta_netif,
                                     SemaphoreHandle_t cfg_mutex) {
    s_cfg = active_cfg;
    s_wifi_netif = wifi_sta_netif;
    s_cfg_mutex = cfg_mutex;
    if (!s_stat_mutex) {
        s_stat_mutex = xSemaphoreCreateMutex();
    }
    memset(&s_status, 0, sizeof(s_status));
    edgehmi_wan_failover_init(&s_failover,
                              active_cfg ? active_cfg->net.failover_policy : NET_POLICY_WIFI_PRIMARY);

    esp_netif_config_t pcfg = ESP_NETIF_DEFAULT_PPP();
    s_ppp_netif = esp_netif_new(&pcfg);
    if (s_ppp_netif) {
        esp_netif_ppp_config_t ppp_params = {
            .ppp_phase_event_enabled = true,
            .ppp_error_event_enabled = true,
        };
        esp_netif_ppp_set_params(s_ppp_netif, &ppp_params);
    }
    esp_event_handler_register(IP_EVENT, IP_EVENT_PPP_GOT_IP, on_ppp_ip_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_PPP_LOST_IP, on_ppp_ip_event, NULL);

    xTaskCreatePinnedToCore(cellular_supervisor_task, "edgehmi_cell", 4096, NULL, 4, NULL, 0);
    return EDGEHMI_OK;
}

void edgehmi_cellular_notify_wifi(bool wifi_has_ip) {
    s_wifi_has_ip = wifi_has_ip;
}

void edgehmi_cellular_get_status(edgehmi_cell_status_t *out_cell,
                                 edgehmi_wan_link_t *out_active_link) {
    if (s_stat_mutex) xSemaphoreTake(s_stat_mutex, portMAX_DELAY);
    if (out_cell) *out_cell = s_status;
    if (out_active_link) *out_active_link = s_failover.active_link;
    if (s_stat_mutex) xSemaphoreGive(s_stat_mutex);
}

edgehmi_err_t edgehmi_cellular_save_apn(const char *apn, const char *generation) {
    (void)generation;
    if (!apn || apn[0] == '\0') return EDGEHMI_ERR_INVALID_ARG;
    nvs_handle_t nh;
    if (nvs_open("edgehmi_cell", NVS_READWRITE, &nh) == ESP_OK) {
        nvs_set_str(nh, "apn", apn);
        nvs_commit(nh);
        nvs_close(nh);
    }
    /* Force modem to re-dial with new APN */
    s_ppp_data_mode = false;
    return EDGEHMI_OK;
}
