/**
 * web/web_server.c — WiFi AP + HTTP REST server
 */
#include "web_server.h"
#include "web_ui.h"
#include "../config.h"
#include "../sdl30_types.h"
#include "../sdl30/sdl30.h"
#include "../survey/job.h"
#include "../survey/survey.h"
#include "../storage/storage.h"
#include "../settings.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "cJSON.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <dirent.h>
#include <sys/stat.h>

static const char *TAG = "WEB";
static httpd_handle_t s_httpd = NULL;

// ─── Externals from main.c ────────────────────────────────────────────────────
extern bool g_sdl_ok;
extern char g_sdl_model[16];
extern char g_sdl_serial[16];
extern char g_sdl_rom[8];
extern int  g_lm_timeouts;

// ─── Settings ─────────────────────────────────────────────────────────────────
static settings_t s_settings;

// ─── Sight state ──────────────────────────────────────────────────────────────
static int s_next_sight = SIGHT_BS;

int  web_get_next_sight(void) { return s_next_sight; }
void web_set_next_sight(int s) { s_next_sight = s; }

// ─── Double-reading state machine (BFFB/BFBF/BBFF) ──────────────────────────
typedef enum {
    DBLR_IDLE=0, DBLR_BS1, DBLR_FS1, DBLR_FS2, DBLR_BS2,
    DBLR_PASS, DBLR_FAIL
} dblr_step_t;

typedef struct {
    dblr_step_t step;
    float bs1, bs1_dist, fs1, fs1_dist;
    float fs2, fs2_dist, bs2, bs2_dist;
    float dh1, dh2, diff_mm;
    bool  passed;
} dblr_state_t;

static dblr_state_t s_dblr = {0};

// Sight sequence for each method and step (0-3)
// BFFB=1: BS FS FS BS
// BFBF=2: BS FS BS FS
// BBFF=3: BS BS FS FS
// static const int dblr_seq[3][4] = {
//     {SIGHT_BS, SIGHT_FS, SIGHT_FS, SIGHT_BS},  // BFFB
//     {SIGHT_BS, SIGHT_FS, SIGHT_BS, SIGHT_FS},  // BFBF
//     {SIGHT_BS, SIGHT_BS, SIGHT_FS, SIGHT_FS},  // BBFF
// };
static const char *dblr_labels[3][4] = {
    {"BS1","FS1","FS2","BS2"},  // BFFB
    {"BS1","FS1","BS2","FS2"},  // BFBF
    {"BS1","BS2","FS1","FS2"},  // BBFF
};

static void dblr_reset(void) {
    memset(&s_dblr, 0, sizeof(s_dblr));
    s_dblr.step = DBLR_BS1;
}


// ─── JSON helpers ─────────────────────────────────────────────────────────────
static bool json_str(const char *body, const char *key, char *out, size_t len)
{
    cJSON *root = cJSON_Parse(body);
    if (!root) return false;
    cJSON *item = cJSON_GetObjectItem(root, key);
    bool ok = false;
    if (item && cJSON_IsString(item)) {
        strncpy(out, item->valuestring, len-1);
        out[len-1] = '\0';
        ok = true;
    }
    cJSON_Delete(root);
    return ok;
}

static bool json_float(const char *body, const char *key, float *out)
{
    cJSON *root = cJSON_Parse(body);
    if (!root) return false;
    cJSON *item = cJSON_GetObjectItem(root, key);
    bool ok = false;
    if (item && cJSON_IsNumber(item)) { *out = (float)item->valuedouble; ok = true; }
    cJSON_Delete(root);
    return ok;
}

static bool json_int(const char *body, const char *key, int *out)
{
    cJSON *root = cJSON_Parse(body);
    if (!root) return false;
    cJSON *item = cJSON_GetObjectItem(root, key);
    bool ok = false;
    if (item && cJSON_IsNumber(item)) { *out = item->valueint; ok = true; }
    cJSON_Delete(root);
    return ok;
}

static bool json_bool(const char *body, const char *key, bool *out)
{
    cJSON *root = cJSON_Parse(body);
    if (!root) return false;
    cJSON *item = cJSON_GetObjectItem(root, key);
    bool ok = false;
    if (item && cJSON_IsBool(item)) { *out = cJSON_IsTrue(item); ok = true; }
    cJSON_Delete(root);
    return ok;
}

static esp_err_t read_body(httpd_req_t *req, char *buf, size_t max)
{
    int len = req->content_len;
    if (len <= 0 || len >= (int)max) { buf[0] = '\0'; return ESP_OK; }
    int n = httpd_req_recv(req, buf, len);
    if (n <= 0) return ESP_FAIL;
    buf[n] = '\0';
    return ESP_OK;
}

static void send_json(httpd_req_t *req, const char *json)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, json);
}

static void send_ok(httpd_req_t *req)  { send_json(req, "{\"ok\":true}"); }
static void send_err(httpd_req_t *req, const char *msg)
{
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"%s\"}", msg);
    send_json(req, buf);
}

// ─── GET / ────────────────────────────────────────────────────────────────────
static esp_err_t h_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    // Send full HTML page with explicit length
    size_t html_sz = sizeof(WEB_UI_HTML) - 1;
    ESP_LOGI(TAG, "Sending HTML: %lu bytes", (unsigned long)html_sz);
    esp_err_t ret = httpd_resp_send(req, WEB_UI_HTML, html_sz);
    ESP_LOGI(TAG, "HTML send result: %d", ret);
    return ESP_OK;
}

// ─── GET /api/status ──────────────────────────────────────────────────────────
static esp_err_t h_status(httpd_req_t *req)
{
    const job_t *job = job_get_info();
    size_t tot = 0, used = 0;
    storage_info(&tot, &used);

    char buf[512];
    int mi = s_settings.obs_method - 1;
    if (mi < 0 || mi > 2) mi = 0;
    const char *next_lbl = (s_settings.obs_method > 0 && s_dblr.step > 0 && s_dblr.step < 5)
        ? dblr_labels[mi][(int)s_dblr.step % 4]
        : sight_str((sight_type_t)s_next_sight);
    snprintf(buf, sizeof(buf),
        "{\"sdl_ok\":%s,\"model\":\"%s\",\"serial\":\"%s\","
        "\"job\":\"%s\",\"hi\":%.4f,\"rl\":%.4f,"
        "\"readings\":%lu,\"points\":%lu,\"bench_rl\":%.4f,"
        "\"next_sight\":\"%s\",\"obs_method\":%d,"
        "\"dblr_step\":%d,\"dblr_passed\":%s,\"dblr_diff_mm\":%.3f,"
        "\"flash_total\":%lu,\"flash_used\":%lu}",
        g_sdl_ok ? "true" : "false",
        g_sdl_model, g_sdl_serial,
        job->name, job->current_hi, job->current_rl,
        (unsigned long)job_get_count(), (unsigned long)(job_get_fs_count()+1), job->bench_rl,
        next_lbl, s_settings.obs_method,
        (int)s_dblr.step,
        s_dblr.passed ? "true" : "false",
        s_dblr.diff_mm,
        (unsigned long)tot, (unsigned long)used);
    send_json(req, buf);
    return ESP_OK;
}

// ─── GET /api/records ─────────────────────────────────────────────────────────
static esp_err_t h_records(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr_chunk(req, "{\"records\":[");

    job_lock();
    const record_t *recs = job_get_records();
    uint32_t count = job_get_count();
    bool first = true;

    for (uint32_t i = 0; i < count; i++) {
        const record_t *r = &recs[i];
        if (!r->valid || r->voided) continue;
        char row[200];
        snprintf(row, sizeof(row),
            "%s{\"index\":%lu,\"sight\":\"%s\","
            "\"staff\":%.4f,\"distance\":%.3f,"
            "\"hi\":%.4f,\"rl\":%.4f}",
            first ? "" : ",",
            (unsigned long)r->index, sight_str(r->sight),
            r->staff, r->distance, r->hi, r->rl);
        httpd_resp_sendstr_chunk(req, row);
        first = false;
    }
    job_unlock();

    httpd_resp_sendstr_chunk(req, "]}");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

// ─── GET /api/jobs ────────────────────────────────────────────────────────────
static esp_err_t h_jobs(httpd_req_t *req)
{
    char jobnames[MAX_JOBS][MAX_JOB_NAME];
    int njobs = storage_list_jobs(jobnames, MAX_JOBS);
    size_t tot = 0, used = 0;
    storage_info(&tot, &used);
    const job_t *active = job_get_info();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    char buf[128];
    snprintf(buf, sizeof(buf),
             "{\"active\":\"%s\",\"total\":%lu,\"used\":%lu,\"jobs\":[",
             active->name, (unsigned long)tot, (unsigned long)used);
    httpd_resp_sendstr_chunk(req, buf);

    for (int i = 0; i < njobs; i++) {
        job_t meta = {0};
        strncpy(meta.name, jobnames[i], MAX_JOB_NAME-1);
        storage_load_meta(&meta);
        size_t sz = storage_csv_size(jobnames[i]);
        char row[160];
        snprintf(row, sizeof(row),
            "%s{\"name\":\"%s\",\"bench_rl\":%.4f,"
            "\"points\":%lu,\"size\":%lu}",
            i ? "," : "",
            jobnames[i], meta.bench_rl,
            (unsigned long)meta.point_count, (unsigned long)sz);
        httpd_resp_sendstr_chunk(req, row);
    }
    httpd_resp_sendstr_chunk(req, "]}");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

// ─── POST /api/measure ────────────────────────────────────────────────────────
static esp_err_t h_measure(httpd_req_t *req)
{
    float staff = 0.0f, distance = 0.0f;
    esp_err_t err = sdl30_measure(&staff, &distance);

    if (err != ESP_OK) {
        g_lm_timeouts++;
        ESP_LOGW(TAG, "LM failed (%d/%d)", g_lm_timeouts, SDL_LM_TIMEOUT_MAX);
        send_err(req, "SDL30 measurement failed");
        return ESP_OK;
    }
    g_lm_timeouts = 0;
    g_sdl_ok = true;

    int method = s_settings.obs_method;

    // ── BF mode (method=0) ────────────────────────────────────────────────
    if (method == 0) {
        char body[64] = {0};
        read_body(req, body, sizeof(body));
        char sight_s[4] = "BS";
        json_str(body, "sight", sight_s, sizeof(sight_s));
        sight_type_t sight = sight_from_str(sight_s);
        job_add_point(sight, staff, distance);
        const job_t *job = job_get_info();
        if (sight == SIGHT_BS)      s_next_sight = SIGHT_FS;
        else if (sight == SIGHT_FS) s_next_sight = SIGHT_BS;
        char resp[256];
        snprintf(resp, sizeof(resp),
            "{\"ok\":true,\"mode\":\"BF\","
            "\"index\":%lu,\"sight\":\"%s\","
            "\"staff\":%.4f,\"distance\":%.3f,"
            "\"hi\":%.4f,\"rl\":%.4f}",
            (unsigned long)job_get_count(),
            sight_str(sight), staff, distance,
            job->current_hi, job->current_rl);
        send_json(req, resp);
        return ESP_OK;
    }

    // ── Double-reading mode (BFFB/BFBF/BBFF) ─────────────────────────────
    int mi = method - 1;  // 0=BFFB, 1=BFBF, 2=BBFF
    if (mi < 0 || mi > 2) mi = 0;

    // Initialize if idle
    if (s_dblr.step == DBLR_IDLE || s_dblr.step == DBLR_PASS || s_dblr.step == DBLR_FAIL)
        dblr_reset();

    // Store reading for current step
    switch (s_dblr.step) {
        case DBLR_BS1: s_dblr.bs1=staff; s_dblr.bs1_dist=distance; s_dblr.step=DBLR_FS1; break;
        case DBLR_FS1: s_dblr.fs1=staff; s_dblr.fs1_dist=distance; s_dblr.step=DBLR_FS2; break;
        case DBLR_FS2: s_dblr.fs2=staff; s_dblr.fs2_dist=distance; s_dblr.step=DBLR_BS2; break;
        case DBLR_BS2:
            s_dblr.bs2=staff; s_dblr.bs2_dist=distance;
            // Calculate check
            // For all methods: dh1=first_BS - first_FS, dh2=second_BS - second_FS
            if (mi == 0) { // BFFB: BS1-FS1, BS2-FS2
                s_dblr.dh1 = s_dblr.bs1 - s_dblr.fs1;
                s_dblr.dh2 = s_dblr.bs2 - s_dblr.fs2;
            } else if (mi == 1) { // BFBF: BS1-FS1, BS2-FS2
                s_dblr.dh1 = s_dblr.bs1 - s_dblr.fs1;
                s_dblr.dh2 = s_dblr.bs2 - s_dblr.fs2;
            } else { // BBFF: BS1-FS1, BS2-FS2 (FS1=fs1, FS2=fs2)
                s_dblr.dh1 = s_dblr.bs1 - s_dblr.fs1;
                s_dblr.dh2 = s_dblr.bs2 - s_dblr.fs2;
            }
            s_dblr.diff_mm = fabsf(s_dblr.dh1 - s_dblr.dh2) * 1000.0f;
            s_dblr.passed  = (s_dblr.diff_mm <= s_settings.max_station_mm);
            s_dblr.step    = s_dblr.passed ? DBLR_PASS : DBLR_FAIL;

            if (s_dblr.passed) {
                // Save all 4 raw readings as BS1/FS1/FS2/BS2
                job_add_point(SIGHT_BS1, s_dblr.bs1, s_dblr.bs1_dist);
                job_add_point(SIGHT_FS1, s_dblr.fs1, s_dblr.fs1_dist);
                job_add_point(SIGHT_FS2, s_dblr.fs2, s_dblr.fs2_dist);
                job_add_point(SIGHT_BS2, s_dblr.bs2, s_dblr.bs2_dist);
                ESP_LOGI(TAG, "%s PASS: diff=%.3fmm "
                         "BS1=%.4f FS1=%.4f FS2=%.4f BS2=%.4f",
                         obs_method_str(method), s_dblr.diff_mm,
                         s_dblr.bs1, s_dblr.fs1, s_dblr.fs2, s_dblr.bs2);
            } else {
                ESP_LOGW(TAG, "%s FAIL: diff=%.3fmm > %.1fmm — repeat setup!",
                         obs_method_str(method),
                         s_dblr.diff_mm, s_settings.max_station_mm);
            }
            break;
        default: dblr_reset(); break;
    }

    // Build response
    const job_t *job = job_get_info();
    int cur_step = (int)s_dblr.step;
    int step_num = (cur_step <= 4) ? cur_step : 4;
    const char *lbl = (step_num > 0 && step_num <= 4) ?
                      dblr_labels[mi][step_num-1] : "---";
    char resp[512];
    snprintf(resp, sizeof(resp),
        "{\"ok\":true,\"mode\":\"%s\","
        "\"dblr_step\":%d,\"dblr_label\":\"%s\","
        "\"staff\":%.4f,\"distance\":%.3f,"
        "\"bs1\":%.4f,\"fs1\":%.4f,\"fs2\":%.4f,\"bs2\":%.4f,"
        "\"dh1\":%.4f,\"dh2\":%.4f,\"diff_mm\":%.3f,"
        "\"passed\":%s,"
        "\"hi\":%.4f,\"rl\":%.4f,"
        "\"index\":%lu}",
        obs_method_str(method),
        (int)s_dblr.step, lbl,
        staff, distance,
        s_dblr.bs1, s_dblr.fs1, s_dblr.fs2, s_dblr.bs2,
        s_dblr.dh1, s_dblr.dh2, s_dblr.diff_mm,
        s_dblr.passed ? "true" : "false",
        job->current_hi, job->current_rl,
        (unsigned long)job_get_count());
    send_json(req, resp);
    return ESP_OK;
}

// ─── POST /api/sight ──────────────────────────────────────────────────────────
static esp_err_t h_sight(httpd_req_t *req)
{
    char body[32] = {0};
    read_body(req, body, sizeof(body));
    char sight_s[4] = {0};
    if (json_str(body, "sight", sight_s, sizeof(sight_s)))
        s_next_sight = sight_from_str(sight_s);
    send_ok(req);
    return ESP_OK;
}

// ─── POST /api/job/new ────────────────────────────────────────────────────────
static esp_err_t h_job_new(httpd_req_t *req)
{
    char body[128] = {0};
    read_body(req, body, sizeof(body));
    char name[MAX_JOB_NAME] = {0};
    float bench = 100.0f;
    if (!json_str(body, "name", name, sizeof(name))) {
        send_err(req, "name required"); return ESP_OK;
    }
    json_float(body, "bench_rl", &bench);
    job_new(name, bench);
    s_next_sight = SIGHT_BS;
    send_ok(req);
    return ESP_OK;
}

// ─── POST /api/job/select ─────────────────────────────────────────────────────
static esp_err_t h_job_select(httpd_req_t *req)
{
    char body[64] = {0};
    read_body(req, body, sizeof(body));
    char name[MAX_JOB_NAME] = {0};
    if (!json_str(body, "name", name, sizeof(name))) {
        send_err(req, "name required"); return ESP_OK;
    }
    job_select(name);
    s_next_sight = SIGHT_BS;
    send_ok(req);
    return ESP_OK;
}

// ─── POST /api/job/bench ──────────────────────────────────────────────────────
static esp_err_t h_job_bench(httpd_req_t *req)
{
    char body[64] = {0};
    read_body(req, body, sizeof(body));
    float bench = 0.0f;
    if (!json_float(body, "bench_rl", &bench)) {
        send_err(req, "bench_rl required"); return ESP_OK;
    }
    job_set_bench(bench);
    send_ok(req);
    return ESP_OK;
}

// ─── POST /api/job/delete ─────────────────────────────────────────────────────
static esp_err_t h_job_delete(httpd_req_t *req)
{
    char body[64] = {0};
    read_body(req, body, sizeof(body));
    char name[MAX_JOB_NAME] = {0};
    if (!json_str(body, "name", name, sizeof(name))) {
        send_err(req, "name required"); return ESP_OK;
    }
    job_delete(name);
    send_ok(req);
    return ESP_OK;
}

// ─── POST /api/record/delete ──────────────────────────────────────────────────
static esp_err_t h_rec_delete(httpd_req_t *req)
{
    char body[64] = {0};
    read_body(req, body, sizeof(body));
    int idx = 0;
    if (!json_int(body, "index", &idx)) {
        send_err(req, "index required"); return ESP_OK;
    }
    esp_err_t err = job_delete_point((uint32_t)idx);
    err == ESP_OK ? send_ok(req) : send_err(req, "not found");
    return ESP_OK;
}

// ─── POST /api/record/sight ───────────────────────────────────────────────────
static esp_err_t h_rec_sight(httpd_req_t *req)
{
    char body[64] = {0};
    read_body(req, body, sizeof(body));
    int idx = 0; char sight_s[4] = {0};
    if (!json_int(body, "index", &idx) ||
        !json_str(body, "sight", sight_s, sizeof(sight_s))) {
        send_err(req, "params required"); return ESP_OK;
    }
    esp_err_t err = job_edit_sight((uint32_t)idx, sight_from_str(sight_s));
    err == ESP_OK ? send_ok(req) : send_err(req, "not found");
    return ESP_OK;
}

// ─── GET /api/download ────────────────────────────────────────────────────────
static esp_err_t h_download(httpd_req_t *req)
{
    char jobname[MAX_JOB_NAME];
    strncpy(jobname, job_get_info()->name, sizeof(jobname)-1);

    char query[64] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char tmp[MAX_JOB_NAME] = {0};
        if (httpd_query_key_value(query, "job", tmp, sizeof(tmp)) == ESP_OK)
            strncpy(jobname, tmp, sizeof(jobname)-1);
    }

    char path[320];
    snprintf(path, sizeof(path), "%s/%s.csv", SPIFFS_BASE, jobname);
    FILE *f = fopen(path, "r");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
        return ESP_FAIL;
    }
    char disp[64];
    snprintf(disp, sizeof(disp), "attachment; filename=\"%s.csv\"", jobname);
    httpd_resp_set_type(req, "text/csv");
    httpd_resp_set_hdr(req, "Content-Disposition", disp);
    char buf[256]; size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        httpd_resp_send_chunk(req, buf, n);
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

// ─── GET /api/misclose ────────────────────────────────────────────────────────
static esp_err_t h_misclose(httpd_req_t *req)
{
    float closing = job_get_info()->bench_rl;
    char query[64] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char tmp[32] = {0};
        if (httpd_query_key_value(query, "closing_rl", tmp, sizeof(tmp)) == ESP_OK)
            closing = strtof(tmp, NULL);
    }

    float sum_bs = 0, sum_fs = 0;
    job_lock();
    float mc = survey_misclose(job_get_records(), job_get_count(),
                                job_get_info()->bench_rl, closing,
                                &sum_bs, &sum_fs);
    float first_rl = job_get_info()->bench_rl;
    float last_rl  = job_get_info()->bench_rl;
    float sum_bs_dist = 0, sum_fs_dist = 0;
    const record_t *recs = job_get_records();
    uint32_t count = job_get_count();
    for (uint32_t i = 0; i < count; i++) {
        if (!recs[i].valid || recs[i].voided) continue;
        if (recs[i].sight == SIGHT_BS) sum_bs_dist += recs[i].distance;
        if (recs[i].sight == SIGHT_FS) sum_fs_dist += recs[i].distance;
        if (first_rl == job_get_info()->bench_rl) first_rl = recs[i].rl;
        last_rl = recs[i].rl;
    }
    job_unlock();

    float bench_rl = job_get_info()->bench_rl;
    // Count FS readings as 'points' (turning points)
    uint32_t fs_count = 0;
    for (uint32_t i = 0; i < count; i++)
        if (recs[i].valid && !recs[i].voided && recs[i].sight == SIGHT_FS) fs_count++;

    char buf[420];
    snprintf(buf, sizeof(buf),
        "{\"misclose\":%.4f,\"sum_bs\":%.4f,\"sum_fs\":%.4f,"
        "\"opening_bm\":%.4f,\"comp_elev\":%.4f,"
        "\"last_rl\":%.4f,"
        "\"readings\":%lu,\"points\":%lu,"
        "\"sum_bs_dist\":%.3f,\"sum_fs_dist\":%.3f}",
        mc, sum_bs, sum_fs,
        bench_rl, bench_rl + sum_bs - sum_fs,
        last_rl,
        (unsigned long)count, (unsigned long)(fs_count+1),
        sum_bs_dist, sum_fs_dist);
    send_json(req, buf);
    return ESP_OK;
}

// ─── GET /api/settings ────────────────────────────────────────────────────────
static esp_err_t h_settings_get(httpd_req_t *req)
{
    char buf[320];
    snprintf(buf, sizeof(buf),
        "{\"obs_method\":%d,\"max_station_mm\":%.2f,"
        "\"max_dist_diff\":%.2f,\"max_cum_diff\":%.2f,"
        "\"min_sight_ht\":%.3f,\"max_sight_dist\":%.1f,"
        "\"alert_on_exceed\":%s,\"block_on_exceed\":%s,"
        "\"baud_rate\":%d}",
        s_settings.obs_method, s_settings.max_station_mm,
        s_settings.max_dist_diff, s_settings.max_cum_diff,
        s_settings.min_sight_ht, s_settings.max_sight_dist,
        s_settings.alert_on_exceed ? "true" : "false",
        s_settings.block_on_exceed ? "true" : "false",
        s_settings.baud_rate);
    send_json(req, buf);
    return ESP_OK;
}

// ─── POST /api/settings ───────────────────────────────────────────────────────
static esp_err_t h_settings_post(httpd_req_t *req)
{
    char body[256] = {0};
    read_body(req, body, sizeof(body));

    cJSON *root = cJSON_Parse(body);
    if (root) {
        cJSON *item;
        if ((item = cJSON_GetObjectItem(root, "obs_method")) && cJSON_IsNumber(item))
            s_settings.obs_method = item->valueint;
        if ((item = cJSON_GetObjectItem(root, "max_station_mm")) && cJSON_IsNumber(item))
            s_settings.max_station_mm = (float)item->valuedouble;
        if ((item = cJSON_GetObjectItem(root, "max_dist_diff")) && cJSON_IsNumber(item))
            s_settings.max_dist_diff = (float)item->valuedouble;
        if ((item = cJSON_GetObjectItem(root, "max_cum_diff")) && cJSON_IsNumber(item))
            s_settings.max_cum_diff = (float)item->valuedouble;
        if ((item = cJSON_GetObjectItem(root, "min_sight_ht")) && cJSON_IsNumber(item))
            s_settings.min_sight_ht = (float)item->valuedouble;
        if ((item = cJSON_GetObjectItem(root, "max_sight_dist")) && cJSON_IsNumber(item))
            s_settings.max_sight_dist = (float)item->valuedouble;
        if ((item = cJSON_GetObjectItem(root, "alert_on_exceed")) && cJSON_IsBool(item))
            s_settings.alert_on_exceed = cJSON_IsTrue(item);
        if ((item = cJSON_GetObjectItem(root, "block_on_exceed")) && cJSON_IsBool(item))
            s_settings.block_on_exceed = cJSON_IsTrue(item);
        if ((item = cJSON_GetObjectItem(root, "baud_rate")) && cJSON_IsNumber(item))
            s_settings.baud_rate = item->valueint;
        cJSON_Delete(root);
    }
    settings_save(&s_settings);
    send_ok(req);
    return ESP_OK;
}

// ─── WiFi init ────────────────────────────────────────────────────────────────
static void wifi_init(void)
{
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    wifi_config_t ap = { .ap = {
        .ssid           = WIFI_AP_SSID,
        .password       = WIFI_AP_PASS,
        .ssid_len       = strlen(WIFI_AP_SSID),
        .channel        = WIFI_AP_CHANNEL,
        .authmode       = WIFI_AUTH_WPA2_PSK,
        .max_connection = WIFI_AP_MAX_CONN,
    }};
    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    esp_wifi_start();
    ESP_LOGI(TAG, "WiFi AP: SSID=%s  IP=%s", WIFI_AP_SSID, WIFI_AP_IP);
}

// ─── GET /api/files ─────────────────────────────────────────────────────────
static esp_err_t h_files(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr_chunk(req, "{\"files\":[");
    DIR *dir = opendir(SPIFFS_BASE);
    bool first = true;
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            char path[320];
            snprintf(path, sizeof(path), "%s/%s", SPIFFS_BASE, ent->d_name);
            struct stat st;
            size_t sz = (stat(path, &st) == 0) ? st.st_size : 0;
            char row[512];
            snprintf(row, sizeof(row), "%s{\"name\":\"%s\",\"size\":%lu}",
                     first ? "" : ",", ent->d_name, (unsigned long)sz);
            httpd_resp_sendstr_chunk(req, row);
            first = false;
        }
        closedir(dir);
    }
    size_t tot = 0, used = 0;
    storage_info(&tot, &used);
    char tail[64];
    snprintf(tail, sizeof(tail), "],\"total\":%lu,\"used\":%lu}",
             (unsigned long)tot, (unsigned long)used);
    httpd_resp_sendstr_chunk(req, tail);
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

// ─── POST /api/dblr_repeat ───────────────────────────────────────────────────
static esp_err_t h_dblr_repeat(httpd_req_t *req)
{
    dblr_reset();
    ESP_LOGI(TAG, "DBLR: setup repeated by user");
    send_ok(req);
    return ESP_OK;
}

// ─── Start server ─────────────────────────────────────────────────────────────
esp_err_t web_server_start(void)
{
    // Load settings
    settings_load(&s_settings);

    wifi_init();

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 20;
    cfg.stack_size = 24576;
    cfg.send_wait_timeout = 30;
    cfg.recv_wait_timeout = 30;

    if (httpd_start(&s_httpd, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server start failed");
        return ESP_FAIL;
    }

    httpd_uri_t uris[] = {
        { "/",                  HTTP_GET,  h_root,          NULL },
        { "/api/status",        HTTP_GET,  h_status,        NULL },
        { "/api/records",       HTTP_GET,  h_records,       NULL },
        { "/api/jobs",          HTTP_GET,  h_jobs,          NULL },
        { "/api/download",      HTTP_GET,  h_download,      NULL },
        { "/api/misclose",      HTTP_GET,  h_misclose,      NULL },
        { "/api/settings",      HTTP_GET,  h_settings_get,  NULL },
        { "/api/measure",       HTTP_POST, h_measure,       NULL },
        { "/api/sight",         HTTP_POST, h_sight,         NULL },
        { "/api/job/new",       HTTP_POST, h_job_new,       NULL },
        { "/api/job/select",    HTTP_POST, h_job_select,    NULL },
        { "/api/job/bench",     HTTP_POST, h_job_bench,     NULL },
        { "/api/job/delete",    HTTP_POST, h_job_delete,    NULL },
        { "/api/record/delete", HTTP_POST, h_rec_delete,    NULL },
        { "/api/record/sight",  HTTP_POST, h_rec_sight,     NULL },
        { "/api/settings",      HTTP_POST, h_settings_post, NULL },
        { "/api/files",         HTTP_GET,  h_files,         NULL },
        { "/api/dblr_repeat",   HTTP_POST, h_dblr_repeat,   NULL },
    };
    for (int i = 0; i < 18; i++)
        httpd_register_uri_handler(s_httpd, &uris[i]);

    ESP_LOGI(TAG, "HTTP ready at http://%s", WIFI_AP_IP);
    return ESP_OK;
}

esp_err_t web_server_stop(void)
{
    if (s_httpd) { httpd_stop(s_httpd); s_httpd = NULL; }
    esp_wifi_stop();
    esp_wifi_deinit();
    return ESP_OK;
}
