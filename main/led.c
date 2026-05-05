#include "led.h"
#include "config.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static volatile led_state_t s_state = LED_BOOTING;

void led_set(led_state_t state) { s_state = state; }

static void led_task(void *pv)
{
    int tick = 0;
    while (1) {
        led_state_t st = s_state;
        int r = 0, y = 0, g = 0;

        switch (st) {
            case LED_BOOTING:
            case LED_NO_SDL:
                // Red slow blink: 500ms on / 500ms off
                r = (tick % 10) < 5;
                break;
            case LED_READY:
                g = 1;
                break;
            case LED_MEASURING:
                // Yellow fast blink: 200ms on / 200ms off
                y = (tick % 4) < 2;
                break;
        }

        gpio_set_level(LED_RED_GPIO, r);
        gpio_set_level(LED_YEL_GPIO, y);
        gpio_set_level(LED_GRN_GPIO, g);
        tick++;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void led_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << LED_RED_GPIO) |
                        (1ULL << LED_YEL_GPIO) |
                        (1ULL << LED_GRN_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    gpio_set_level(LED_RED_GPIO, 0);
    gpio_set_level(LED_YEL_GPIO, 0);
    gpio_set_level(LED_GRN_GPIO, 0);

    xTaskCreate(led_task, "led", 1024, NULL, 1, NULL);
}
