/**
 * led.h — Traffic-light status LEDs, active-LOW (VCC=GPIO15, Red=GPIO9, Yellow=GPIO10, Green=GPIO11)
 *
 * States:
 *   LED_BOOTING   — Red slow blink: system starting up
 *   LED_NO_SDL    — Red slow blink: SDL30 not connected
 *   LED_READY     — Green solid:    SDL30 connected, WiFi up
 *   LED_MEASURING — Yellow fast blink: measurement in progress
 */
#pragma once

typedef enum {
    LED_BOOTING,
    LED_NO_SDL,
    LED_READY,
    LED_MEASURING,
} led_state_t;

void led_init(void);
void led_set(led_state_t state);
