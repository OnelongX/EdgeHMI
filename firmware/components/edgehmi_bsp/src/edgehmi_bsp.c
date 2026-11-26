#include "edgehmi_bsp.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include <string.h>

static const char *TAG = "edgehmi_bsp";
static lv_display_t *s_disp = NULL;
static lv_indev_t *s_indev = NULL;
static esp_lcd_panel_handle_t s_panel = NULL;
static esp_lcd_touch_handle_t s_touch = NULL;
static i2c_master_bus_handle_t s_i2c_bus = NULL;
#if defined(CONFIG_EDGEHMI_BOARD_WAVESHARE_43)
static i2c_master_dev_handle_t s_ch422g_out = NULL;
#endif

/* Pre-verified pinouts for popular ESP32-S3 800x480 industrial RGB boards */
#if defined(CONFIG_EDGEHMI_BOARD_WAVESHARE_43)
static const edgehmi_bsp_info_t s_info = {
    .board_name = "Waveshare ESP32-S3-Touch-LCD-4.3",
    .width = 800,
    .height = 480,
    .default_rs485_tx = 44,
    .default_rs485_rx = 43,
    .default_rs485_de = -1,
    .default_can_tx = 20,
    .default_can_rx = 19,
    .backlight_gpio = -1, /* Controlled via CH422G I2C expander on Waveshare 4.3 */
};
#define BSP_LCD_DE     5
#define BSP_LCD_VSYNC  3
#define BSP_LCD_HSYNC  46
#define BSP_LCD_PCLK   7
#define BSP_LCD_DATA_PINS { 14, 38, 18, 17, 10, 39, 0, 45, 48, 47, 21, 1, 2, 42, 41, 40 }
#define BSP_TOUCH_SDA  8
#define BSP_TOUCH_SCL  9

#elif defined(CONFIG_EDGEHMI_BOARD_SUNTON_8048S043) || defined(CONFIG_EDGEHMI_BOARD_SUNTON_8048S070)
static const edgehmi_bsp_info_t s_info = {
#if defined(CONFIG_EDGEHMI_BOARD_SUNTON_8048S070)
    .board_name = "Sunton ESP32-8048S070 (7.0-inch)",
#else
    .board_name = "Sunton ESP32-8048S043 (4.3-inch)",
#endif
    .width = 800,
    .height = 480,
    .default_rs485_tx = 17,
    .default_rs485_rx = 18,
    .default_rs485_de = 16,
    .default_can_tx = 12,
    .default_can_rx = 11,
    .backlight_gpio = 2,
};
#define BSP_LCD_DE     40
#define BSP_LCD_VSYNC  41
#define BSP_LCD_HSYNC  39
#define BSP_LCD_PCLK   42
#define BSP_LCD_DATA_PINS { 8, 3, 46, 9, 1, 5, 6, 7, 15, 16, 4, 45, 48, 47, 21, 14 }
#define BSP_TOUCH_SDA  19
#define BSP_TOUCH_SCL  20

#else
static const edgehmi_bsp_info_t s_info = {
    .board_name = "EdgeHMI Headless / Custom",
    .width = 800,
    .height = 480,
    .default_rs485_tx = 17,
    .default_rs485_rx = 18,
    .default_rs485_de = 16,
    .default_can_tx = 20,
    .default_can_rx = 19,
    .backlight_gpio = -1,
};
#endif

static uint32_t lvgl_tick_get_cb(void) {
    return (uint32_t)(esp_timer_get_time() / 1000LL);
}

#ifndef CONFIG_EDGEHMI_BOARD_HEADLESS
static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    if (s_panel) {
        esp_lcd_panel_draw_bitmap(s_panel, area->x1, area->y1,
                                  area->x2 + 1, area->y2 + 1, px_map);
    }
    lv_display_flush_ready(disp);
}

static void lvgl_touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data) {
    (void)indev;
    if (!s_touch) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    esp_lcd_touch_read_data(s_touch);
    uint16_t tx[1] = {0};
    uint16_t ty[1] = {0};
    uint8_t cnt = 0;
    if (esp_lcd_touch_get_coordinates(s_touch, tx, ty, NULL, &cnt, 1) && cnt > 0) {
        data->point.x = (int32_t)tx[0];
        data->point.y = (int32_t)ty[0];
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

#if defined(CONFIG_EDGEHMI_BOARD_WAVESHARE_43)
static void waveshare_ch422g_enable(void) {
    /* CH422G I2C IO expander: 0x24 = set EXIO output mode; 0x38 = pull TP_RST, LCD_BL, LCD_RST high */
    i2c_master_dev_handle_t mode_dev = NULL;
    i2c_device_config_t d_mode = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = 0x24,
        .scl_speed_hz = 400000,
    };
    i2c_device_config_t d_out = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = 0x38,
        .scl_speed_hz = 400000,
    };
    if (i2c_master_bus_add_device(s_i2c_bus, &d_mode, &mode_dev) == ESP_OK &&
        i2c_master_bus_add_device(s_i2c_bus, &d_out, &s_ch422g_out) == ESP_OK) {
        uint8_t mode_val = 0x01u;
        uint8_t rst_low  = 0x0Au; /* TP_RST=0, LCD_BL=1, LCD_RST=1 */
        uint8_t all_on   = 0x0Eu; /* EXIO1(TP_RST)=1, EXIO2(LCD_BL)=1, EXIO3(LCD_RST)=1 */
        i2c_master_transmit(mode_dev, &mode_val, 1, 50);
        i2c_master_transmit(s_ch422g_out, &rst_low, 1, 50);
        vTaskDelay(pdMS_TO_TICKS(20));
        i2c_master_transmit(s_ch422g_out, &all_on, 1, 50);
        vTaskDelay(pdMS_TO_TICKS(50));
        i2c_master_bus_rm_device(mode_dev);
        ESP_LOGI(TAG, "Waveshare CH422G expander enabled (LCD backlight + GT911 reset)");
    }
}
#endif

static void init_touch_gt911(void) {
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = BSP_TOUCH_SDA,
        .scl_io_num = BSP_TOUCH_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&bus_cfg, &s_i2c_bus) != ESP_OK) {
        ESP_LOGW(TAG, "Touch I2C0 master bus init failed");
        return;
    }

#if defined(CONFIG_EDGEHMI_BOARD_WAVESHARE_43)
    waveshare_ch422g_enable();
#endif

    esp_lcd_panel_io_handle_t tp_io = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    tp_io_cfg.scl_speed_hz = 400000;
    if (esp_lcd_new_panel_io_i2c(s_i2c_bus, &tp_io_cfg, &tp_io) != ESP_OK) {
        ESP_LOGW(TAG, "GT911 panel IO create failed");
        return;
    }

    esp_lcd_touch_config_t tp_cfg = {
        .x_max = s_info.width,
        .y_max = s_info.height,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = GPIO_NUM_NC,
        .levels = { .reset = 0, .interrupt = 0 },
        .flags = { .swap_xy = 0, .mirror_x = 0, .mirror_y = 0 },
    };
    if (esp_lcd_touch_new_i2c_gt911(tp_io, &tp_cfg, &s_touch) == ESP_OK) {
        s_indev = lv_indev_create();
        lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(s_indev, lvgl_touch_read_cb);
        ESP_LOGI(TAG, "GT911 touch registered as LVGL 9 pointer input");
    } else {
        ESP_LOGW(TAG, "GT911 touch probe failed (continuing display-only)");
    }
}
#endif

static void init_backlight_pwm(void) {
    if (s_info.backlight_gpio < 0) {
        return;
    }
    ledc_timer_config_t tc = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&tc);

    ledc_channel_config_t cc = {
        .gpio_num = s_info.backlight_gpio,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
    };
    ledc_channel_config(&cc);
    edgehmi_bsp_set_backlight(CONFIG_EDGEHMI_BACKLIGHT_DEFAULT_PCT);
}

void edgehmi_bsp_set_backlight(uint8_t percent) {
#if defined(CONFIG_EDGEHMI_BOARD_WAVESHARE_43)
    if (s_ch422g_out) {
        uint8_t val = (percent > 0) ? 0x0Eu : 0x0Au;
        i2c_master_transmit(s_ch422g_out, &val, 1, 50);
    }
#endif
    if (s_info.backlight_gpio < 0) {
        return;
    }
    if (percent > 100) percent = 100;
    uint32_t duty = (uint32_t)percent * 255u / 100u;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

edgehmi_err_t edgehmi_bsp_init(void) {
    lv_init();
    lv_tick_set_cb(lvgl_tick_get_cb);
    ESP_LOGI(TAG, "Initializing BSP: %s (%ux%u)",
             s_info.board_name, (unsigned)s_info.width, (unsigned)s_info.height);

#ifndef CONFIG_EDGEHMI_BOARD_HEADLESS
    esp_lcd_rgb_panel_config_t pcfg = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {
            .pclk_hz = CONFIG_EDGEHMI_LCD_PCLK_HZ,
            .h_res = s_info.width,
            .v_res = s_info.height,
            .hsync_pulse_width = 4,
            .hsync_back_porch = 8,
            .hsync_front_porch = 8,
            .vsync_pulse_width = 4,
            .vsync_back_porch = 8,
            .vsync_front_porch = 8,
            .flags.pclk_active_neg = true,
        },
        .data_width = 16,
        .bits_per_pixel = 16,
        .num_fbs = 2,
        .bounce_buffer_size_px = s_info.width * 10,
        .psram_trans_align = 64,
        .hsync_gpio_num = BSP_LCD_HSYNC,
        .vsync_gpio_num = BSP_LCD_VSYNC,
        .de_gpio_num = BSP_LCD_DE,
        .pclk_gpio_num = BSP_LCD_PCLK,
        .disp_gpio_num = -1,
        .data_gpio_nums = BSP_LCD_DATA_PINS,
        .flags.fb_in_psram = true,
    };

    if (esp_lcd_new_rgb_panel(&pcfg, &s_panel) != ESP_OK ||
        esp_lcd_panel_reset(s_panel) != ESP_OK ||
        esp_lcd_panel_init(s_panel) != ESP_OK) {
        ESP_LOGE(TAG, "RGB LCD panel init failed");
        return EDGEHMI_ERR_IO;
    }

    s_disp = lv_display_create(s_info.width, s_info.height);
    size_t buf_bytes = (size_t)s_info.width * 40u * sizeof(uint16_t);
    void *buf1 = heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    void *buf2 = heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf1) {
        ESP_LOGE(TAG, "Failed to allocate LVGL draw buffer in PSRAM");
        return EDGEHMI_ERR_IO;
    }
    lv_display_set_buffers(s_disp, buf1, buf2, (uint32_t)buf_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(s_disp, lvgl_flush_cb);

    init_touch_gt911();
    init_backlight_pwm();
#endif

    return EDGEHMI_OK;
}

const edgehmi_bsp_info_t *edgehmi_bsp_get_info(void) {
    return &s_info;
}

lv_display_t *edgehmi_bsp_get_display(void) {
    return s_disp;
}
