/**
 * main.c — SDL30 Collector entry point
 *
 * LA policy:
 *   - Once at startup
 *   - After SDL_LM_TIMEOUT_MAX consecutive LM timeouts
 *   - Never polled periodically
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "config.h"
#include "led.h"
#include "sdl30/sdl30.h"
#include "survey/job.h"
#include "storage/storage.h"
#include "web/web_server.h"

static const char *TAG = "MAIN";

// ─── SDL30 connection state (shared with web_server) ─────────────────────────
bool  g_sdl_ok        = false;
char  g_sdl_model[16] = "---";
char  g_sdl_serial[16]= "---";
char  g_sdl_rom[8]    = "---";
int   g_lm_timeouts   = 0;

// ─── SDL30 monitor task ───────────────────────────────────────────────────────
static void sdl_monitor_task(void *pv)
{
    vTaskDelay(pdMS_TO_TICKS(1000));
    ESP_LOGI(TAG, "SDL30 startup check...");

    if (sdl30_get_info(g_sdl_model, g_sdl_serial, g_sdl_rom) == ESP_OK) {
        g_sdl_ok = true;
        ESP_LOGI(TAG, "SDL30 connected: %s SN:%s ROM:%s",
                 g_sdl_model, g_sdl_serial, g_sdl_rom);
        led_set(LED_READY);
    } else {
        g_sdl_ok = false;
        ESP_LOGW(TAG, "SDL30 not found on startup");
        led_set(LED_NO_SDL);
    }

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(500));
        if (g_lm_timeouts >= SDL_LM_TIMEOUT_MAX) {
            ESP_LOGI(TAG, "%d LM timeouts — checking SDL30...", SDL_LM_TIMEOUT_MAX);
            if (sdl30_get_info(g_sdl_model, g_sdl_serial, g_sdl_rom) == ESP_OK) {
                g_sdl_ok = true;
                ESP_LOGI(TAG, "SDL30 alive: %s SN:%s", g_sdl_model, g_sdl_serial);
                led_set(LED_READY);
            } else {
                g_sdl_ok = false;
                ESP_LOGW(TAG, "SDL30 disconnected!");
                led_set(LED_NO_SDL);
            }
            g_lm_timeouts = 0;
        }
    }
}

// ─── app_main ─────────────────────────────────────────────────────────────────
void app_main(void)
{
    ESP_LOGI(TAG, "=== SDL30 Collector starting ===");
    ESP_LOGI(TAG, "ESP32-S3-N16  ESP-IDF v5.4");

    led_init();   // Red slow blink until SDL30 confirmed

    nvs_flash_init();

    ESP_LOGI(TAG, "Mounting SPIFFS...");
    if (storage_init() != ESP_OK)
        ESP_LOGW(TAG, "SPIFFS failed — running without storage");

    sdl30_init();
    job_init();
    web_server_start();

    ESP_LOGI(TAG, "=== Ready! ===");
    ESP_LOGI(TAG, "WiFi: %s  pass: %s", WIFI_AP_SSID, WIFI_AP_PASS);
    ESP_LOGI(TAG, "Open: http://%s", WIFI_AP_IP);

    xTaskCreate(sdl_monitor_task, "sdl_mon", 4096, NULL, TASK_PRIO_SDL, NULL);
}
