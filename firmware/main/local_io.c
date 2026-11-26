#include "local_io.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "edgehmi_lio";
static adc_oneshot_unit_handle_t s_adc1 = NULL;

static void local_io_save_retentive(const char *tag_name, double val) {
    nvs_handle_t h;
    if (nvs_open("edgehmi_ret", NVS_READWRITE, &h) == ESP_OK) {
        char key[16];
        edgehmi_nvs_safe_key(tag_name, key);
        nvs_set_blob(h, key, &val, sizeof(val));
        nvs_commit(h);
        nvs_close(h);
    }
}

static bool gpio_to_adc1_channel(int8_t pin, adc_channel_t *out_ch) {
    /* ESP32-S3 ADC1 maps GPIO1..GPIO10 -> ADC_CHANNEL_0..ADC_CHANNEL_9 */
    if (pin >= 1 && pin <= 10) {
        *out_ch = (adc_channel_t)(pin - 1);
        return true;
    }
    return false;
}

edgehmi_err_t edgehmi_local_io_configure(const edgehmi_config_t *cfg) {
    if (!cfg || cfg->local_io_count == 0) {
        return EDGEHMI_OK;
    }

    uint8_t pwm_ch = 1; /* Channel 0 reserved for LCD backlight */
    for (size_t i = 0; i < cfg->local_io_count; ++i) {
        const edgehmi_local_io_t *io = &cfg->local_io[i];
        if (io->gpio_pin < 0 || io->gpio_pin > 48) {
            continue;
        }
        if (io->mode == LOCAL_IO_DI) {
            gpio_config_t gc = {
                .pin_bit_mask = (1ULL << io->gpio_pin),
                .mode = GPIO_MODE_INPUT,
                .pull_up_en = io->active_low ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
                .pull_down_en = io->active_low ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE,
                .intr_type = GPIO_INTR_DISABLE,
            };
            gpio_config(&gc);
        } else if (io->mode == LOCAL_IO_DO) {
            gpio_config_t gc = {
                .pin_bit_mask = (1ULL << io->gpio_pin),
                .mode = GPIO_MODE_OUTPUT,
                .pull_up_en = GPIO_PULLUP_DISABLE,
                .pull_down_en = GPIO_PULLDOWN_DISABLE,
                .intr_type = GPIO_INTR_DISABLE,
            };
            gpio_config(&gc);
            int init_lvl = (io->default_val > 0.5) ? 1 : 0;
            if (io->active_low) init_lvl = !init_lvl;
            gpio_set_level((gpio_num_t)io->gpio_pin, init_lvl);
        } else if (io->mode == LOCAL_IO_ADC) {
            adc_channel_t ch = ADC_CHANNEL_0;
            if (gpio_to_adc1_channel(io->gpio_pin, &ch)) {
                if (!s_adc1) {
                    adc_oneshot_unit_init_cfg_t init_cfg = { .unit_id = ADC_UNIT_1 };
                    adc_oneshot_new_unit(&init_cfg, &s_adc1);
                }
                if (s_adc1) {
                    adc_oneshot_chan_cfg_t ccfg = {
                        .bitwidth = ADC_BITWIDTH_12,
                        .atten = ADC_ATTEN_DB_12,
                    };
                    adc_oneshot_config_channel(s_adc1, ch, &ccfg);
                }
            }
        } else if (io->mode == LOCAL_IO_PWM && pwm_ch < LEDC_CHANNEL_MAX) {
            ledc_timer_config_t tc = {
                .speed_mode = LEDC_LOW_SPEED_MODE,
                .duty_resolution = LEDC_TIMER_10_BIT,
                .timer_num = LEDC_TIMER_1,
                .freq_hz = 2000,
                .clk_cfg = LEDC_AUTO_CLK,
            };
            ledc_timer_config(&tc);
            ledc_channel_config_t cc = {
                .gpio_num = io->gpio_pin,
                .speed_mode = LEDC_LOW_SPEED_MODE,
                .channel = (ledc_channel_t)pwm_ch++,
                .timer_sel = LEDC_TIMER_1,
                .duty = 0,
                .hpoint = 0,
            };
            ledc_channel_config(&cc);
        }
    }
    ESP_LOGI(TAG, "Configured %u native ESP32-S3 local_io pins", (unsigned)cfg->local_io_count);
    return EDGEHMI_OK;
}

void edgehmi_local_io_sync(const edgehmi_config_t *cfg, edgehmi_bus_t *bus, uint64_t now_ms) {
    if (!cfg || !bus) {
        return;
    }
    uint8_t pwm_ch = 1;
    for (size_t i = 0; i < cfg->local_io_count; ++i) {
        const edgehmi_local_io_t *io = &cfg->local_io[i];
        if (io->gpio_pin < 0 || io->tag_idx < 0) {
            continue;
        }
        if (io->mode == LOCAL_IO_DI) {
            int raw = gpio_get_level((gpio_num_t)io->gpio_pin);
            if (io->active_low) raw = !raw;
            edgehmi_bus_set(bus, io->tag_idx, raw ? 1.0 : 0.0, TAG_QUAL_GOOD, now_ms);
        } else if (io->mode == LOCAL_IO_DO) {
            double wv = 0.0;
            if (edgehmi_bus_pop_write(bus, io->tag_idx, &wv)) {
                int lvl = (wv > 0.5) ? 1 : 0;
                if (io->active_low) lvl = !lvl;
                gpio_set_level((gpio_num_t)io->gpio_pin, lvl);
                double applied_v = (wv > 0.5) ? 1.0 : 0.0;
                edgehmi_bus_set(bus, io->tag_idx, applied_v, TAG_QUAL_GOOD, now_ms);
                if (io->retentive) local_io_save_retentive(io->tag_name, applied_v);
            }
        } else if (io->mode == LOCAL_IO_ADC) {
            adc_channel_t ch = ADC_CHANNEL_0;
            int raw = 0;
            if (s_adc1 && gpio_to_adc1_channel(io->gpio_pin, &ch) &&
                adc_oneshot_read(s_adc1, ch, &raw) == ESP_OK) {
                double scale = (io->scale != 0.0) ? io->scale : 1.0;
                double eng = (double)raw * scale + io->offset;
                edgehmi_bus_set(bus, io->tag_idx, eng, TAG_QUAL_GOOD, now_ms);
            }
        } else if (io->mode == LOCAL_IO_PWM && pwm_ch < LEDC_CHANNEL_MAX) {
            double wv = 0.0;
            if (edgehmi_bus_pop_write(bus, io->tag_idx, &wv)) {
                if (wv < 0.0) wv = 0.0;
                if (wv > 100.0) wv = 100.0;
                uint32_t duty = (uint32_t)(wv * 1023.0 / 100.0);
                ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)pwm_ch, duty);
                ledc_update_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)pwm_ch);
                edgehmi_bus_set(bus, io->tag_idx, wv, TAG_QUAL_GOOD, now_ms);
                if (io->retentive) local_io_save_retentive(io->tag_name, wv);
            }
            pwm_ch++;
        }
    }
}
