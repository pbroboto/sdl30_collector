/**
 * sdl30/sdl30.c — SDL30 digital level communication
 *
 * Confirmed real SDL30 response formats (SN:001786, ROM:1112):
 *   LA\r → "LA SDL30,001786,1112\r\n"
 *   LM\r → "LM 0.7890,1.88\r\n"   (space after LM, no + sign)
 *   LT\r → no response (silent, Single mode)
 *   LB\r → "LB 0,0\r\n"
 */
#include "sdl30.h"
#include "../config.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "SDL30";
static SemaphoreHandle_t s_uart_mtx = NULL;

// ─── Init ─────────────────────────────────────────────────────────────────────
esp_err_t sdl30_init(void)
{
    s_uart_mtx = xSemaphoreCreateMutex();

    uart_config_t cfg = {
        .baud_rate  = SDL_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(SDL_UART_NUM,
                                        SDL_UART_BUF * 2, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(SDL_UART_NUM, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(SDL_UART_NUM,
                                  SDL_GPIO_TX, SDL_GPIO_RX,
                                  UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_LOGI(TAG, "UART%d ready @ %d baud  RX=GPIO%d TX=GPIO%d",
             SDL_UART_NUM, SDL_BAUD, SDL_GPIO_RX, SDL_GPIO_TX);

    // Set Single measurement mode (LXa)
    char buf[16];
    vTaskDelay(pdMS_TO_TICKS(200));
    sdl30_raw_cmd("LXa\r", buf, sizeof(buf));
    ESP_LOGI(TAG, "LXa (Single mode): %s", buf);

    // Set display resolution to 0.0001m (L/B 0,0)
    vTaskDelay(pdMS_TO_TICKS(200));
    sdl30_raw_cmd("L/B 0,0\r", buf, sizeof(buf));
    ESP_LOGI(TAG, "L/B 0,0 (0.0001m resolution): %s", buf);

    return ESP_OK;
}

// ─── Raw command ──────────────────────────────────────────────────────────────
esp_err_t sdl30_raw_cmd(const char *cmd, char *resp, size_t rlen)
{
    if (!s_uart_mtx) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_uart_mtx, portMAX_DELAY);

    uart_flush_input(SDL_UART_NUM);
    uart_write_bytes(SDL_UART_NUM, cmd, strlen(cmd));

    memset(resp, 0, rlen);
    size_t idx = 0;
    TickType_t t0 = xTaskGetTickCount();
    TickType_t last_byte = t0;
    bool got = false;

    while ((xTaskGetTickCount() - t0) < pdMS_TO_TICKS(SDL_TIMEOUT_MS)) {
        if ((xTaskGetTickCount() - last_byte) > pdMS_TO_TICKS(3000) && idx > 0) break;
        uint8_t b;
        if (uart_read_bytes(SDL_UART_NUM, &b, 1, pdMS_TO_TICKS(100)) > 0) {
            last_byte = xTaskGetTickCount();
            if (b == 0x06) { strncpy(resp, "ACK", rlen-1); got = true; break; }
            if (b == 0x15) { strncpy(resp, "NAK", rlen-1); got = true; break; }
            if (idx < rlen-1) resp[idx++] = (char)b;
            if (b == '\n') {
                resp[idx] = '\0';
                // Strip \r\n and trailing spaces
                for (int i = (int)idx-1; i >= 0; i--) {
                    if (resp[i] == '\r' || resp[i] == '\n' || resp[i] == ' ')
                        resp[i] = '\0';
                    else break;
                }
                got = true;
                break;
            }
        }
        taskYIELD();
    }

    xSemaphoreGive(s_uart_mtx);

    if (!got) {
        strncpy(resp, "TIMEOUT", rlen-1);
        ESP_LOGW(TAG, "Timeout: %s", cmd);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

// ─── Get info ─────────────────────────────────────────────────────────────────
esp_err_t sdl30_get_info(char *model, char *serial, char *rom)
{
    char buf[64];
    esp_err_t err = sdl30_raw_cmd("LA\r", buf, sizeof(buf));
    if (err != ESP_OK) return err;
    if (strncmp(buf, "LA", 2) != 0) return ESP_ERR_INVALID_RESPONSE;

    // Format: "LA SDL30,001786,1112"
    char m[16] = {0}, s[16] = {0}, r[8] = {0};
    if (sscanf(buf + 3, "%15[^,],%15[^,],%7s", m, s, r) >= 2) {
        if (model)  strncpy(model,  m, 15);
        if (serial) strncpy(serial, s, 15);
        if (rom)    strncpy(rom,    r,  7);
        ESP_LOGI(TAG, "SDL30: model=%s serial=%s ROM=%s", m, s, r);
        return ESP_OK;
    }
    return ESP_ERR_INVALID_RESPONSE;
}

// ─── Measure ─────────────────────────────────────────────────────────────────
esp_err_t sdl30_measure(float *staff, float *distance)
{
    char buf[64];
    esp_err_t err = sdl30_raw_cmd("LM\r", buf, sizeof(buf));
    if (err != ESP_OK) return err;

    // Must start with "LM"
    if (strncmp(buf, "LM", 2) != 0) {
        ESP_LOGE(TAG, "Unexpected response: [%s]", buf);
        return ESP_ERR_INVALID_RESPONSE;
    }

    // Error code: "LM Exxx"
    if (buf[3] == 'E') {
        ESP_LOGW(TAG, "SDL30 error: %s", buf);
        return ESP_ERR_INVALID_RESPONSE;
    }

    // Real SDL30 format: "LM 0.7890,1.88"
    //   buf+3 skips "LM " → "0.7890,1.88"
    float st = 0.0f, dist = 0.0f;
    if (sscanf(buf + 3, "%f,%f", &st, &dist) != 2) {
        ESP_LOGE(TAG, "Parse failed: [%s]", buf);
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (staff)    *staff    = st;
    if (distance) *distance = dist;

    ESP_LOGI(TAG, "Measured: staff=%.4fm  dist=%.3fm", st, dist);
    // Note: LT not needed in Single mode — SDL30 stops automatically
    return ESP_OK;
}

// ─── Stop ─────────────────────────────────────────────────────────────────────
esp_err_t sdl30_stop(void)
{
    char buf[16];
    // LT returns no response in Single mode — TIMEOUT is expected
    sdl30_raw_cmd("LT\r", buf, sizeof(buf));
    return ESP_OK;
}

// ─── Get params ───────────────────────────────────────────────────────────────
esp_err_t sdl30_get_params(int *res1, int *res2)
{
    char buf[32];
    esp_err_t err = sdl30_raw_cmd("LB\r", buf, sizeof(buf));
    if (err != ESP_OK) return err;
    if (strncmp(buf, "LB", 2) != 0) return ESP_ERR_INVALID_RESPONSE;

    int r1 = 0, r2 = 0;
    if (sscanf(buf + 3, "%d,%d", &r1, &r2) >= 1) {
        if (res1) *res1 = r1;
        if (res2) *res2 = r2;
        return ESP_OK;
    }
    return ESP_ERR_INVALID_RESPONSE;
}
