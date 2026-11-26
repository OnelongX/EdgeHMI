#include "edgehmi_bsp.h"

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/ledc.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include <string.h>

static const char *TAG = "edgehmi_bsp";
static lv_display_t *s_disp = NULL;
static esp_lcd_panel_handle_t s_panel = NULL;

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

#ifndef CONFIG_EDGEHMI_BOARD_HEADLESS
static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    if (s_panel) {
        esp_lcd_panel_draw_bitmap(s_panel, area->x1, area->y1,
                                  area->x2 + 1, area->y2 + 1, px_map);
    }
    lv_display_flush_ready(disp);
}

static void init_touch_i2c_bus(void) {
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = BSP_TOUCH_SDA,
        .scl_io_num = BSP_TOUCH_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 400000,
    };
    if (i2c_param_config(I2C_NUM_0, &conf) == ESP_OK) {
        i2c_driver_install(I2C_NUM_0, conf.mode, 0, 0, 0);
        ESP_LOGI(TAG, "Touch I2C0 ready (SDA=%d SCL=%d, GT911 addr 0x5D/0x14)",
                 BSP_TOUCH_SDA, BSP_TOUCH_SCL);
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

    init_touch_i2c_bus();
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
