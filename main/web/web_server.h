/**
 * web/web_server.h — WiFi AP + HTTP REST server
 *
 * API endpoints:
 *   GET  /                    web app (single page)
 *   GET  /api/status          current job state JSON
 *   GET  /api/records         all records JSON
 *   GET  /api/jobs            list all jobs + storage info
 *   GET  /api/download        download active job CSV
 *   GET  /api/download?job=X  download specific job CSV
 *   GET  /api/misclose?closing_rl=X  misclose report
 *   POST /api/measure         {sight} trigger SDL30
 *   POST /api/sight           {sight} set next sight only
 *   POST /api/job/new         {name, bench_rl}
 *   POST /api/job/select      {name}
 *   POST /api/job/bench       {bench_rl}
 *   POST /api/job/delete      {name}
 *   POST /api/record/delete   {index}
 *   POST /api/record/sight    {index, sight}
 */
#pragma once
#include "esp_err.h"

/** Start WiFi AP and HTTP server. Call once after job_init(). */
esp_err_t web_server_start(void);

/** Stop HTTP server and WiFi AP. */
esp_err_t web_server_stop(void);

/** Get next sight type set by web UI. */
int web_get_next_sight(void);

/** Set next sight type (called after auto-advance). */
void web_set_next_sight(int sight);
