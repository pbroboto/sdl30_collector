/**
 * survey/job.c — Job and measurement point management
 */
#include "job.h"
#include "survey.h"
#include "../storage/storage.h"
#include "../config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "JOB";

static job_t            s_job;
static record_t         s_records[MAX_POINTS];
static uint32_t         s_count = 0;
static void update_current_from_records(void);
static SemaphoreHandle_t s_mtx  = NULL;

static void update_current_from_records(void);
// ─── Init ─────────────────────────────────────────────────────────────────────
#define ACTIVE_JOB_FILE SPIFFS_BASE "/active_job.txt"

static void save_active_job_name(void)
{
    FILE *f = fopen(ACTIVE_JOB_FILE, "w");
    if (!f) return;
    fprintf(f, "%s\n", s_job.name);
    fclose(f);
}

static void load_active_job_name(char *name, size_t len)
{
    FILE *f = fopen(ACTIVE_JOB_FILE, "r");
    if (!f) { strncpy(name, "JOB_001", len-1); return; }
    if (!fgets(name, len, f)) strncpy(name, "JOB_001", len-1);
    // Strip newline
    for (int i = strlen(name)-1; i >= 0; i--)
        if (name[i] == '\n' || name[i] == '\r') name[i] = '\0';
    fclose(f);
}

esp_err_t job_init(void)
{
    s_mtx = xSemaphoreCreateMutex();
    memset(&s_job, 0, sizeof(s_job));
    memset(s_records, 0, sizeof(s_records));

    // Try to restore last active job from SPIFFS
    char last_name[MAX_JOB_NAME] = {0};
    load_active_job_name(last_name, sizeof(last_name));
    strncpy(s_job.name, last_name, MAX_JOB_NAME - 1);
    s_job.bench_rl   = 100.000f;
    s_job.current_hi = 100.000f;
    s_job.current_rl = 100.000f;
    s_count = 0;

    // Load meta and records from SPIFFS
    if (storage_load_meta(&s_job) == ESP_OK) {
        s_count = storage_load_records(&s_job, s_records, MAX_POINTS);
        update_current_from_records();
        ESP_LOGI(TAG, "Restored job: %s  BM=%.4f  %lu records",
                 s_job.name, s_job.bench_rl, (unsigned long)s_count);
    } else {
        ESP_LOGI(TAG, "No saved job found — using default: %s", s_job.name);
    }
    return ESP_OK;
}

// ─── Helpers ──────────────────────────────────────────────────────────────────
static void update_current_from_records(void)
{
    // Restore current HI/RL from last valid non-voided record
    s_job.current_hi = s_job.bench_rl;
    s_job.current_rl = s_job.bench_rl;
    for (int i = (int)s_count - 1; i >= 0; i--) {
        if (s_records[i].valid && !s_records[i].voided) {
            s_job.current_hi = s_records[i].hi;
            s_job.current_rl = s_records[i].rl;
            break;
        }
    }
}

// ─── New job ──────────────────────────────────────────────────────────────────
esp_err_t job_new(const char *name, float bench_rl)
{
    if (!name || strlen(name) == 0) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    memset(&s_job, 0, sizeof(s_job));
    memset(s_records, 0, sizeof(s_records));

    strncpy(s_job.name, name, MAX_JOB_NAME - 1);
    s_job.bench_rl   = bench_rl;
    s_job.current_hi = bench_rl;
    s_job.current_rl = bench_rl;
    s_job.point_count = 0;
    s_count = 0;
    xSemaphoreGive(s_mtx);

    storage_save_meta(&s_job);
    save_active_job_name();
    // Create empty CSV so job appears in list
    storage_rewrite_csv(&s_job, NULL, 0);
    ESP_LOGI(TAG, "New job: %s  BM=%.4f", name, bench_rl);
    return ESP_OK;
}

// ─── Select existing job ──────────────────────────────────────────────────────
esp_err_t job_select(const char *name)
{
    if (!name || strlen(name) == 0) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    memset(&s_job, 0, sizeof(s_job));
    memset(s_records, 0, sizeof(s_records));

    strncpy(s_job.name, name, MAX_JOB_NAME - 1);
    storage_load_meta(&s_job);
    s_count = storage_load_records(&s_job, s_records, MAX_POINTS);
    s_job.point_count = s_count;
    update_current_from_records();
    xSemaphoreGive(s_mtx);

    save_active_job_name();
    ESP_LOGI(TAG, "Selected job: %s  %lu records  BM=%.4f",
             name, (unsigned long)s_count, s_job.bench_rl);
    return ESP_OK;
}

// ─── Delete job ───────────────────────────────────────────────────────────────
esp_err_t job_delete(const char *name)
{
    storage_delete_job(name);

    // If deleting active job reset to defaults
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    if (strcmp(name, s_job.name) == 0) {
        strncpy(s_job.name, "JOB_001", MAX_JOB_NAME - 1);
        s_job.bench_rl   = 100.000f;
        s_job.current_hi = 100.000f;
        s_job.current_rl = 100.000f;
        s_job.point_count = 0;
        s_count = 0;
        memset(s_records, 0, sizeof(s_records));
    }
    xSemaphoreGive(s_mtx);
    return ESP_OK;
}

// ─── Set benchmark ────────────────────────────────────────────────────────────
esp_err_t job_set_bench(float bench_rl)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_job.bench_rl = bench_rl;
    survey_recalc(s_records, s_count, bench_rl);
    update_current_from_records();
    xSemaphoreGive(s_mtx);

    storage_rewrite_csv(&s_job, s_records, s_count);
    storage_save_meta(&s_job);
    ESP_LOGI(TAG, "Benchmark set to %.4f  — all RLs recalculated", bench_rl);
    return ESP_OK;
}

// ─── Add point ────────────────────────────────────────────────────────────────
esp_err_t job_add_point(sight_type_t sight, float staff, float distance)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);

    if (s_count >= MAX_POINTS) {
        xSemaphoreGive(s_mtx);
        ESP_LOGE(TAG, "Record buffer full (%d points)", MAX_POINTS);
        return ESP_ERR_NO_MEM;
    }

    // Calculate HI / RL
    float hi = s_job.current_hi;
    float rl = s_job.current_rl;

    switch (sight) {
        case SIGHT_BS:
        case SIGHT_BS1:
            hi = rl + staff;
            break;
        case SIGHT_BS2:
            // BS2 is a check reading - instrument hasn't moved, keep same HI
            break;
        case SIGHT_IS:
        case SIGHT_FS:
        case SIGHT_FS1:
        case SIGHT_FS2:
            rl = hi - staff;
            break;
    }

    record_t r = {
        .index    = s_count + 1,
        .sight    = sight,
        .staff    = staff,
        .distance = distance,
        .hi       = hi,
        .rl       = rl,
        .voided   = false,
        .valid    = true,
    };

    s_records[s_count++] = r;
    s_job.current_hi   = hi;
    s_job.current_rl   = rl;
    s_job.point_count  = s_count;

    xSemaphoreGive(s_mtx);

    storage_append_record(&s_job, &r);
    storage_save_meta(&s_job);

    ESP_LOGI(TAG, "Added #%lu [%s] staff=%.4f dist=%.3f RL=%.4f",
             (unsigned long)(unsigned long)r.index, sight_str(sight), staff, distance, rl);
    return ESP_OK;
}

// ─── Delete point ─────────────────────────────────────────────────────────────
esp_err_t job_delete_point(uint32_t index)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);

    bool found = false;
    for (uint32_t i = 0; i < s_count; i++) {
        if (s_records[i].index == index && s_records[i].valid) {
            // Shift array left
            for (uint32_t j = i; j < s_count - 1; j++)
                s_records[j] = s_records[j + 1];
            s_count--;
            // Renumber
            for (uint32_t j = i; j < s_count; j++)
                s_records[j].index = j + 1;
            found = true;
            break;
        }
    }

    if (found) {
        survey_recalc(s_records, s_count, s_job.bench_rl);
        s_job.point_count = s_count;
        update_current_from_records();
    }
    xSemaphoreGive(s_mtx);

    if (!found) return ESP_ERR_NOT_FOUND;

    storage_rewrite_csv(&s_job, s_records, s_count);
    storage_save_meta(&s_job);
    ESP_LOGI(TAG, "Deleted point #%lu — %lu records remain", (unsigned long)index, (unsigned long)s_count);
    return ESP_OK;
}

// ─── Edit sight ───────────────────────────────────────────────────────────────
esp_err_t job_edit_sight(uint32_t index, sight_type_t new_sight)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    bool found = false;
    for (uint32_t i = 0; i < s_count; i++) {
        if (s_records[i].index == index && s_records[i].valid) {
            s_records[i].sight = new_sight;
            found = true;
            break;
        }
    }
    if (found) {
        survey_recalc(s_records, s_count, s_job.bench_rl);
        update_current_from_records();
    }
    xSemaphoreGive(s_mtx);

    if (!found) return ESP_ERR_NOT_FOUND;

    storage_rewrite_csv(&s_job, s_records, s_count);
    ESP_LOGI(TAG, "Point #%lu sight changed to %s", (unsigned long)(unsigned long)index, sight_str(new_sight));
    return ESP_OK;
}

// ─── Getters ──────────────────────────────────────────────────────────────────
const job_t    *job_get_info(void)    { return &s_job; }
const record_t *job_get_records(void) { return s_records; }
uint32_t        job_get_count(void)   { return s_count; }
uint32_t job_get_fs_count(void) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < s_count; i++)
        if (s_records[i].valid && !s_records[i].voided
            && s_records[i].sight == SIGHT_FS) n++;
    return n;
}
uint32_t job_get_bs2_count(void) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < s_count; i++)
        if (s_records[i].valid && !s_records[i].voided
            && s_records[i].sight == SIGHT_BS2) n++;
    return n;
}
void            job_lock(void)        { xSemaphoreTake(s_mtx, portMAX_DELAY); }
void            job_unlock(void)      { xSemaphoreGive(s_mtx); }
