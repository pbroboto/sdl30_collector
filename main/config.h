/**
 * config.h — SDL30 Collector project-wide configuration
 */
#pragma once

// ─── SDL30 UART ───────────────────────────────────────────────────────────────
#define SDL_UART_NUM        UART_NUM_1
#define SDL_BAUD            2400        // confirmed: 2400 works with SDL30
#define SDL_GPIO_RX         GPIO_NUM_16 // SP3232EEN TTL TXD (white wire)
#define SDL_GPIO_TX         GPIO_NUM_17 // SP3232EEN TTL RXD (green wire)
#define SDL_TIMEOUT_MS      15000
#define SDL_UART_BUF        512
#define SDL_LM_TIMEOUT_MAX  3           // send LA after this many LM timeouts

// ─── WiFi AP ──────────────────────────────────────────────────────────────────
#define WIFI_AP_SSID        "SDL30_Collector"
#define WIFI_AP_PASS        "survey1234"
#define WIFI_AP_IP          "192.168.4.1"
#define WIFI_AP_CHANNEL     6
#define WIFI_AP_MAX_CONN    4

// ─── SPIFFS ───────────────────────────────────────────────────────────────────
#define SPIFFS_BASE         "/spiffs"
#define SPIFFS_MAX_FILES    10

// ─── Survey limits ────────────────────────────────────────────────────────────
#define MAX_POINTS          1000
#define MAX_JOB_NAME        24
#define MAX_JOBS            50

// ─── Settings defaults ────────────────────────────────────────────────────────
#define DEF_OBS_METHOD      0           // 0=BF 1=BFFB 2=BFBF 3=BBFF
#define DEF_MAX_STATION_MM  2.0f        // mm
#define DEF_MAX_DIST_DIFF   5.0f        // m
#define DEF_MAX_CUM_DIFF    10.0f       // m
#define DEF_MIN_SIGHT_HT    0.3f        // m
#define DEF_MAX_SIGHT_DIST  50.0f       // m
#define DEF_ALERT_ON        true
#define DEF_BLOCK_ON        false

// ─── Hardware ─────────────────────────────────────────────────────────────────
// Temporary: running on ESP32-WROOM-32 while S3 GPIO issue is investigated
#define LED_VCC_GPIO        GPIO_NUM_21  // permanent HIGH — feeds all LED anodes
#define LED_RED_GPIO        GPIO_NUM_22  // confirmed working on WROOM-32
#define LED_YEL_GPIO        GPIO_NUM_19
#define LED_GRN_GPIO        GPIO_NUM_18

// ─── FreeRTOS task priorities ─────────────────────────────────────────────────
#define TASK_PRIO_SDL       5
#define TASK_PRIO_APP       4
