#ifndef EDGEHMI_BSP_H
#define EDGEHMI_BSP_H

#include "edgehmi_types.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *board_name;
    uint16_t width;
    uint16_t height;
    int8_t default_rs485_tx;
    int8_t default_rs485_rx;
    int8_t default_rs485_de;
    int8_t default_can_tx;
    int8_t default_can_rx;
    int8_t backlight_gpio;
} edgehmi_bsp_info_t;

/* Initialize ESP32-S3 RGB LCD (double-buffered PSRAM + bounce buffer), GT911 touch & LVGL 9 display */
edgehmi_err_t edgehmi_bsp_init(void);

/* Set LCD backlight brightness (0..100%) via LEDC PWM */
void edgehmi_bsp_set_backlight(uint8_t percent);

/* Query active board profile & default industrial bus pinout */
const edgehmi_bsp_info_t *edgehmi_bsp_get_info(void);

/* Return LVGL 9 display handle created by BSP */
lv_display_t *edgehmi_bsp_get_display(void);

#ifdef __cplusplus
}
#endif

#endif /* EDGEHMI_BSP_H */
