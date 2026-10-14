#include "drv_beep.h"

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/ledc.h"
#include "esp_log.h"

static const char *TAG = "beep";

#define BEEP_GPIO        CONFIG_PROG_BEEP_GPIO
#define BEEP_FREQ_HI     2930    /* resonant frequency of the buzzer */
#define BEEP_FREQ_LO     1500
#define BEEP_DUTY        2048    /* 25% of 8191 (13-bit) */

typedef struct {
    uint16_t freq;
    uint16_t on_ms;
    uint16_t off_ms;
} beep_step_t;

/* Pattern tables, terminated by a zero-length step */
static const beep_step_t s_patterns[][4] = {
    [BEEP_PAT_KEY]   = {{BEEP_FREQ_HI, 40, 0}},
    [BEEP_PAT_BOOT]  = {{BEEP_FREQ_HI, 120, 0}},
    [BEEP_PAT_START] = {{BEEP_FREQ_HI, 60, 60}, {BEEP_FREQ_HI, 60, 0}},
    [BEEP_PAT_DONE]  = {{BEEP_FREQ_HI, 60, 60}, {BEEP_FREQ_HI, 60, 60}, {BEEP_FREQ_HI, 120, 0}},
    [BEEP_PAT_FAIL]  = {{BEEP_FREQ_LO, 400, 0}},
    [BEEP_PAT_ERROR] = {{BEEP_FREQ_LO, 100, 80}, {BEEP_FREQ_LO, 100, 0}},
};

static QueueHandle_t s_queue;

static void beep_hw_init(void)
{
    const ledc_timer_config_t timer = {
        .duty_resolution = LEDC_TIMER_13_BIT,
        .freq_hz = BEEP_FREQ_HI,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_1,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&timer);

    const ledc_channel_config_t channel = {
        .channel = LEDC_CHANNEL_1,
        .gpio_num = BEEP_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_sel = LEDC_TIMER_1,
        .duty = 0,
        .hpoint = 0,
    };
    ledc_channel_config(&channel);
}

static void beep_tone(uint16_t freq, bool on)
{
    if (on) {
        ledc_set_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_1, freq);
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, BEEP_DUTY);
    } else {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, 0);
    }
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
}

static void beep_task(void *arg)
{
    (void)arg;
    beep_pattern_t pat;
    while (1) {
        if (xQueueReceive(s_queue, &pat, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (pat >= (beep_pattern_t)(sizeof(s_patterns) / sizeof(s_patterns[0]))) {
            continue;
        }
        const beep_step_t *step = s_patterns[pat];
        while (step->on_ms) {
            beep_tone(step->freq, true);
            vTaskDelay(pdMS_TO_TICKS(step->on_ms));
            beep_tone(0, false);
            if (step->off_ms) {
                vTaskDelay(pdMS_TO_TICKS(step->off_ms));
            }
            step++;
        }
    }
}

void beep_init(void)
{
    beep_hw_init();
    s_queue = xQueueCreate(6, sizeof(beep_pattern_t));
    configASSERT(s_queue);
    xTaskCreate(beep_task, "beep_task", 2048, NULL, 4, NULL);
    ESP_LOGI(TAG, "buzzer ready on GPIO%d", BEEP_GPIO);
}

void beep_play(beep_pattern_t pat)
{
    if (s_queue) {
        xQueueSend(s_queue, &pat, 0);   /* drop when full */
    }
}
