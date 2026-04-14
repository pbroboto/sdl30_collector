/**
 * sdl30/sdl30.h — SDL30 digital level communication
 *
 * Protocol (from SDL30 manual section 14):
 *   Baud: 1200 or 2400, 8N1, no flow control, RS-232C levels
 *
 *   Commands sent TO SDL30:
 *     "LM\r"  → start measurement
 *     "LT\r"  → stop measurement
 *     "LA\r"  → get instrument info
 *     "LB\r"  → get parameters
 *
 *   Responses FROM SDL30:
 *     "LM +1.2345, 023.456\r\n"  → staff(m), distance(m)
 *     "LM Exxx\r\n"              → error code
 *     "LA SDL30, 123456, 0100\r\n" → model, serial, ROM
 *     "LB 0, 0\r\n"              → resolution params
 *     ACK (0x06)                 → command accepted
 *     NAK (0x15)                 → command rejected
 *
 *   IMPORTANT: SDL30 only accepts commands in Status Mode or Menu Mode.
 */
#pragma once
#include "esp_err.h"

/**
 * Initialise UART2 for SDL30 communication.
 * Call once at startup before any other sdl30_* functions.
 */
esp_err_t sdl30_init(void);

/**
 * Query instrument info (model, serial number, ROM version).
 * Sends "LA\r", parses "LA SDL30, 123456, 0100\r\n".
 * Returns ESP_OK if SDL30 responds correctly.
 *
 * @param model   output buffer, min 16 bytes
 * @param serial  output buffer, min 16 bytes
 * @param rom     output buffer, min 8 bytes
 */
esp_err_t sdl30_get_info(char *model, char *serial, char *rom);

/**
 * Trigger one measurement.
 * Sends "LM\r", waits for response, sends "LT\r" to stop.
 * Returns ESP_OK on success.
 *
 * @param staff     output: staff reading in metres (signed)
 * @param distance  output: distance in metres
 */
esp_err_t sdl30_measure(float *staff, float *distance);

/**
 * Stop ongoing measurement.
 * Sends "LT\r".
 */
esp_err_t sdl30_stop(void);

/**
 * Get measurement parameters.
 * Sends "LB\r", parses "LB 0, 0\r\n".
 */
esp_err_t sdl30_get_params(int *res1, int *res2);

/**
 * Send a raw command and get raw response.
 * Useful for debugging.
 *
 * @param cmd   command string including \r
 * @param resp  output buffer for response
 * @param rlen  response buffer size
 */
esp_err_t sdl30_raw_cmd(const char *cmd, char *resp, size_t rlen);
