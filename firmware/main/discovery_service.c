#include "discovery_service.h"

#include "discovery_proto.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "mdns.h"
#include "ota_firmware.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "edgehmi_disc";

typedef struct {
    const edgehmi_config_t *cfg;
    edgehmi_bus_t *bus;
    SemaphoreHandle_t cfg_mutex;
    uint16_t http_port;
    char device_id[32];
} disc_ctx_t;

static disc_ctx_t s_ctx;

static void udp_discovery_task(void *arg) {
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "UDP socket create failed");
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(EDGEHMI_DISCOVERY_UDP_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "UDP bind port %d failed", EDGEHMI_DISCOVERY_UDP_PORT);
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "UDP Discovery responder listening on port %d", EDGEHMI_DISCOVERY_UDP_PORT);
    char rx[256];
    char tx[384];

    for (;;) {
        struct sockaddr_in src;
        socklen_t slen = sizeof(src);
        int len = recvfrom(sock, rx, sizeof(rx) - 1, 0, (struct sockaddr *)&src, &slen);
        if (len > 0 && edgehmi_discovery_is_probe(rx, (size_t)len)) {
            edgehmi_device_info_t info;
            memset(&info, 0, sizeof(info));
            strncpy(info.device_id, s_ctx.device_id, sizeof(info.device_id) - 1);
            strncpy(info.fw_version, EDGEHMI_FW_VERSION, sizeof(info.fw_version) - 1);
            info.http_port = s_ctx.http_port;
            info.uptime_s = (uint32_t)(esp_timer_get_time() / 1000000LL);

            if (s_ctx.cfg_mutex) xSemaphoreTake(s_ctx.cfg_mutex, portMAX_DELAY);
            strncpy(info.name, (s_ctx.cfg && s_ctx.cfg->device_name[0]) ? s_ctx.cfg->device_name : "EdgeHMI",
                    sizeof(info.name) - 1);
            strncpy(info.cfg_version, (s_ctx.cfg && s_ctx.cfg->version[0]) ? s_ctx.cfg->version : "0.0.0",
                    sizeof(info.cfg_version) - 1);
            info.width = (s_ctx.cfg && s_ctx.cfg->width > 0) ? s_ctx.cfg->width : 800;
            info.height = (s_ctx.cfg && s_ctx.cfg->height > 0) ? s_ctx.cfg->height : 480;

            if (s_ctx.bus) {
                for (size_t i = 0; i < s_ctx.bus->count; ++i) {
                    edgehmi_quality_t q = TAG_QUAL_INIT;
                    edgehmi_bus_get(s_ctx.bus, (int16_t)i, NULL, &q);
                    if (q == TAG_QUAL_STALE || q == TAG_QUAL_BAD) {
                        info.comm_loss = true;
                        break;
                    }
                }
            }
            if (s_ctx.cfg_mutex) xSemaphoreGive(s_ctx.cfg_mutex);

            size_t out_len = edgehmi_discovery_build_announce(&info, tx, sizeof(tx));
            if (out_len > 0) {
                sendto(sock, tx, out_len, 0, (struct sockaddr *)&src, slen);
            }
        }
    }
}

edgehmi_err_t edgehmi_discovery_start(const edgehmi_config_t *active_cfg,
                                      edgehmi_bus_t *bus,
                                      SemaphoreHandle_t cfg_mutex,
                                      uint16_t http_port) {
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_ctx.device_id, sizeof(s_ctx.device_id),
             "edgehmi-%02x%02x%02x", mac[3], mac[4], mac[5]);
    s_ctx.cfg = active_cfg;
    s_ctx.bus = bus;
    s_ctx.cfg_mutex = cfg_mutex;
    s_ctx.http_port = http_port;

    /* Start mDNS advertisement: http://edgehmi-xxxxxx.local */
    if (mdns_init() == ESP_OK) {
        mdns_hostname_set(s_ctx.device_id);
        mdns_instance_name_set(active_cfg ? active_cfg->device_name : "EdgeHMI");
        mdns_txt_item_t txt[] = {
            {"fw", EDGEHMI_FW_VERSION},
            {"cfg", active_cfg ? active_cfg->version : "1.0.0"},
            {"id", s_ctx.device_id},
        };
        mdns_service_add(NULL, "_edgehmi", "_tcp", http_port, txt, 3);
        ESP_LOGI(TAG, "mDNS registered: %s.local (_edgehmi._tcp:%u)",
                 s_ctx.device_id, (unsigned)http_port);
    }

    xTaskCreatePinnedToCore(udp_discovery_task, "edgehmi_disc", 4096, NULL, 3, NULL, 0);
    return EDGEHMI_OK;
}
