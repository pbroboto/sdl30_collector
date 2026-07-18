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
        float carry = survey_recalc(s_records, s_count, s_job.bench_rl);
        update_current_from_records();
        s_job.current_rl = carry;
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
    s_job.current_setup_no = 0;
    for (int i = (int)s_count - 1; i >= 0; i--) {
        if (s_records[i].valid && !s_records[i].voided) {
            s_job.current_hi = s_records[i].hi;
            s_job.current_rl = s_records[i].rl;
            break;
        }
    }
    // current_setup_no must be max across ALL records — not just the last one.
    // After a mid-array insertion the inserted setup gets a high setup_no but
    // sits before lower-numbered setups, so the last record's setup_no is not
    // the max and the next job_add_point(BS1) would collide.
    for (uint32_t i = 0; i < s_count; i++) {
        if (s_records[i].valid && s_records[i].setup_no > s_job.current_setup_no)
            s_job.current_setup_no = s_records[i].setup_no;
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
    float carry = survey_recalc(s_records, s_count, s_job.bench_rl);
    update_current_from_records();
    s_job.current_rl = carry;
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
    float carry = survey_recalc(s_records, s_count, bench_rl);
    update_current_from_records();
    s_job.current_rl = carry;
    xSemaphoreGive(s_mtx);

    storage_rewrite_csv(&s_job, s_records, s_count);
    storage_save_meta(&s_job);
    ESP_LOGI(TAG, "Benchmark set to %.4f  — all RLs recalculated", bench_rl);
    return ESP_OK;
}

// ─── Add point ────────────────────────────────────────────────────────────────
esp_err_t job_add_point(sight_type_t sight, float staff, float distance,
                        const char *override_name)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);

    if (s_count >= MAX_POINTS) {
        xSemaphoreGive(s_mtx);
        ESP_LOGE(TAG, "Record buffer full (%d points)", MAX_POINTS);
        return ESP_ERR_NO_MEM;
    }

    if (sight == SIGHT_BS || sight == SIGHT_BS1)
        s_job.current_setup_no++;


    // Calculate HI / RL
    float hi = s_job.current_hi;
    float rl = s_job.current_rl;
    float carry_rl = rl;  // rl to propagate to next setup (may differ from r->rl for BS2)

    switch (sight) {
        case SIGHT_BS:
        case SIGHT_BS1:
            hi = rl + staff;
            carry_rl = rl;
            break;
        case SIGHT_BS2: {
            // Store sinking check RL in the record
            rl = hi - staff;
            carry_rl = rl;  // fallback if mean can't be computed
            // Compute mean RL to carry forward: prev_BM + (bs_mean - fs_mean)
            float bs1_staff = 0.0f, fs1_staff = 0.0f, fs2_staff = 0.0f;
            float prev_bm_rl = s_job.current_rl;  // RL at last BS1 position (approx)
            int found_bs1 = 0, found_fs2 = 0, found_fs1 = 0;
            for (int j = (int)s_count - 1; j >= 0; j--) {
                if (!s_records[j].valid || s_records[j].voided) continue;
                if (!found_fs2 && s_records[j].sight == SIGHT_FS2) {
                    fs2_staff = s_records[j].staff; found_fs2 = 1;
                } else if (!found_fs1 && found_fs2 && s_records[j].sight == SIGHT_FS1) {
                    fs1_staff = s_records[j].staff; found_fs1 = 1;
                } else if (!found_bs1 && s_records[j].sight == SIGHT_BS1) {
                    bs1_staff  = s_records[j].staff;
                    prev_bm_rl = s_records[j].rl;  // RL before this setup
                    found_bs1  = 1;
                }
                if (found_bs1 && found_fs1 && found_fs2) break;
            }
            if (found_bs1 && found_fs1 && found_fs2) {
                float bs_mean = (bs1_staff + staff) / 2.0f;
                float fs_mean = (fs1_staff + fs2_staff) / 2.0f;
                carry_rl = prev_bm_rl + (bs_mean - fs_mean);
            }
            break;
        }
        case SIGHT_IS:
        case SIGHT_FS:
        case SIGHT_FS1:
        case SIGHT_FS2:
            rl = hi - staff;
            carry_rl = rl;
            break;
    }

    record_t r = {
        .index    = s_count + 1,
        .setup_no = s_job.current_setup_no,
        .sight    = sight,
        .staff    = staff,
        .distance = distance,
        .hi       = hi,
        .rl       = rl,
        .voided   = false,
        .valid    = true,
    };

    // ─── Auto-generate point name ─────────────────────────────────────
    r.name[0] = '\0';
    if (sight == SIGHT_BS || sight == SIGHT_BS1) {
        // First BS of job → BM001, else copy from previous FS/FS2
        int has_prev_bs = 0;
        for (uint32_t i = 0; i < s_count; i++) {
            if (s_records[i].valid && !s_records[i].voided &&
                (s_records[i].sight == SIGHT_BS || s_records[i].sight == SIGHT_BS1)) {
                has_prev_bs = 1; break;
            }
        }
        if (!has_prev_bs) {
            strcpy(r.name, "BM001");
        } else {
            // Copy from most recent FS/FS2
            for (int i = (int)s_count - 1; i >= 0; i--) {
                if (s_records[i].valid && !s_records[i].voided &&
                    (s_records[i].sight == SIGHT_FS || s_records[i].sight == SIGHT_FS2)) {
                    strncpy(r.name, s_records[i].name, MAX_POINT_NAME-1);
                    r.name[MAX_POINT_NAME-1] = '\0';
                    break;
                }
            }
        }
        if (override_name && override_name[0] != '\0') {
            strncpy(r.name, override_name, MAX_POINT_NAME-1);
            r.name[MAX_POINT_NAME-1] = '\0';
        }
    } else if (sight == SIGHT_BS2) {
        // Copy from BS1 of current setup (previous BS1 record)
        for (int i = (int)s_count - 1; i >= 0; i--) {
            if (s_records[i].valid && !s_records[i].voided &&
                s_records[i].sight == SIGHT_BS1) {
                strncpy(r.name, s_records[i].name, MAX_POINT_NAME-1);
                r.name[MAX_POINT_NAME-1] = '\0';
                break;
            }
        }
    } else if (sight == SIGHT_FS || sight == SIGHT_FS1) {
        if (override_name && override_name[0] != '\0') {
            strncpy(r.name, override_name, MAX_POINT_NAME-1);
            r.name[MAX_POINT_NAME-1] = '\0';
        } else {
            // Sequential auto-name: max existing TP number + 1
            int max_tp = 0;
            for (uint32_t i = 0; i < s_count; i++) {
                if (s_records[i].valid && !s_records[i].voided) {
                    int tp_num = 0;
                    if (sscanf(s_records[i].name, "TP%d", &tp_num) == 1) {
                        if (tp_num > max_tp) max_tp = tp_num;
                    }
                }
            }
            snprintf(r.name, MAX_POINT_NAME, "TP%03d", max_tp + 1);
        }
    } else if (sight == SIGHT_FS2) {
        // Copy from FS1 of current setup
        for (int i = (int)s_count - 1; i >= 0; i--) {
            if (s_records[i].valid && !s_records[i].voided &&
                s_records[i].sight == SIGHT_FS1) {
                strncpy(r.name, s_records[i].name, MAX_POINT_NAME-1);
                r.name[MAX_POINT_NAME-1] = '\0';
                break;
            }
        }
    } else if (sight == SIGHT_IS) {
        // Auto-increment IS name
        int max_is = 0;
        for (uint32_t i = 0; i < s_count; i++) {
            if (s_records[i].valid && !s_records[i].voided) {
                int is_num = 0;
                if (sscanf(s_records[i].name, "IS%d", &is_num) == 1) {
                    if (is_num > max_is) max_is = is_num;
                }
            }
        }
        snprintf(r.name, MAX_POINT_NAME, "IS%03d", max_is + 1);
        if (override_name && override_name[0] != '\0') {
            strncpy(r.name, override_name, MAX_POINT_NAME-1);
            r.name[MAX_POINT_NAME-1] = '\0';
        }
    }

    s_records[s_count++] = r;
    s_job.current_hi   = hi;
    s_job.current_rl   = carry_rl;
    s_job.point_count  = s_count;

    xSemaphoreGive(s_mtx);

    storage_append_record(&s_job, &r);
    storage_save_meta(&s_job);

    ESP_LOGI(TAG, "Added #%lu [%s] staff=%.4f dist=%.3f RL=%.4f",
             (unsigned long)r.index, sight_str(sight), staff, distance, rl);
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
        float carry = survey_recalc(s_records, s_count, s_job.bench_rl);
        s_job.point_count = s_count;
        update_current_from_records();
        s_job.current_rl = carry;
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
        float carry = survey_recalc(s_records, s_count, s_job.bench_rl);
        update_current_from_records();
        s_job.current_rl = carry;
    }
    xSemaphoreGive(s_mtx);

    if (!found) return ESP_ERR_NOT_FOUND;

    storage_rewrite_csv(&s_job, s_records, s_count);
    ESP_LOGI(TAG, "Point #%lu sight changed to %s", (unsigned long)index, sight_str(new_sight));
    return ESP_OK;
}

// ─── Delete setup ─────────────────────────────────────────────────────────────
esp_err_t job_delete_setup(uint32_t setup_no)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    uint32_t write_pos = 0, removed = 0;
    for (uint32_t i = 0; i < s_count; i++) {
        if (s_records[i].setup_no == setup_no) {
            removed++;
        } else {
            s_records[write_pos++] = s_records[i];
        }
    }
    s_count = write_pos;
    for (uint32_t i = 0; i < s_count; i++)
        s_records[i].index = i + 1;
    float carry = survey_recalc(s_records, s_count, s_job.bench_rl);
    s_job.point_count = s_count;
    update_current_from_records();
    s_job.current_rl = carry;
    xSemaphoreGive(s_mtx);

    storage_rewrite_csv(&s_job, s_records, s_count);
    storage_save_meta(&s_job);
    ESP_LOGI(TAG, "Deleted setup_no=%lu: %lu records removed, %lu remain",
             (unsigned long)setup_no, (unsigned long)removed, (unsigned long)s_count);
    return ESP_OK;
}

// ─── Insert point ─────────────────────────────────────────────────────────────
esp_err_t job_insert_point(uint32_t after_index, sight_type_t sight, float staff,
                           float distance, const char *name, uint32_t setup_no,
                           float *hi_out, float *rl_out)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    if (s_count >= MAX_POINTS) {
        xSemaphoreGive(s_mtx);
        ESP_LOGE(TAG, "Record buffer full");
        return ESP_ERR_NO_MEM;
    }

    // Find insert position: one slot after the record with index == after_index
    uint32_t pos = 0;
    for (uint32_t i = 0; i < s_count; i++) {
        if (s_records[i].index == after_index) {
            pos = i + 1;
            break;
        }
    }

    // Shift records right
    for (uint32_t i = s_count; i > pos; i--)
        s_records[i] = s_records[i - 1];
    s_count++;

    record_t r = {
        .index    = 0,
        .setup_no = setup_no,
        .sight    = sight,
        .staff    = staff,
        .distance = distance,
        .hi       = 0.0f,
        .rl       = 0.0f,
        .voided   = false,
        .valid    = true,
    };
    r.name[0] = '\0';
    if (name && name[0]) {
        strncpy(r.name, name, MAX_POINT_NAME - 1);
        r.name[MAX_POINT_NAME - 1] = '\0';
    }
    s_records[pos] = r;

    // Renumber all records
    for (uint32_t i = 0; i < s_count; i++)
        s_records[i].index = i + 1;

    float carry = survey_recalc(s_records, s_count, s_job.bench_rl);
    s_job.point_count = s_count;
    update_current_from_records();
    s_job.current_rl = carry;

    if (hi_out) *hi_out = s_records[pos].hi;
    if (rl_out) *rl_out = s_records[pos].rl;

    xSemaphoreGive(s_mtx);

    storage_rewrite_csv(&s_job, s_records, s_count);
    storage_save_meta(&s_job);
    ESP_LOGI(TAG, "Inserted [%s] after_idx=%lu pos=%lu name=%s",
             sight_str(sight), (unsigned long)after_index,
             (unsigned long)(pos + 1), name ? name : "");
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

// ─── Edit point name ──────────────────────────────────────────────────────────
esp_err_t job_edit_name(uint32_t index, const char *name)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    esp_err_t ret = ESP_ERR_NOT_FOUND;
    sight_type_t edited_sight = SIGHT_BS;
    uint32_t edited_pos = 0;

    for (uint32_t i = 0; i < s_count; i++) {
        if (s_records[i].index == index && s_records[i].valid && !s_records[i].voided) {
            strncpy(s_records[i].name, name, MAX_POINT_NAME-1);
            s_records[i].name[MAX_POINT_NAME-1] = '\0';
            edited_sight = s_records[i].sight;
            edited_pos   = i;
            ret = ESP_OK;
            break;
        }
    }

    /*
     * Propagate the name to all records that refer to the same physical point:
     *
     *   FS1_N / FS2_N  ──► FS2_N / FS1_N  (within-setup pair)
     *                  ──► BS1_{N+1} / BS2_{N+1}  (same TP, next setup rear)
     *
     *   BS1_N / BS2_N  ──► BS2_N / BS1_N  (within-setup pair)
     *                  ──► FS1_{N-1} / FS2_{N-1}  (same TP, previous setup fwd)
     *
     *   BF FS  ──► next BS     BF BS  ──► previous FS
     *
     * Search is positional, not by setup_no, so it works on old CSV data.
     */
    if (ret == ESP_OK) {

#define SYNC(j) do { strncpy(s_records[j].name, name, MAX_POINT_NAME-1); \
                     s_records[j].name[MAX_POINT_NAME-1] = '\0'; } while(0)

        switch (edited_sight) {

        /* ── BFFB BS1: sync BS2 forward, then prev-setup FS2+FS1 backward ── */
        case SIGHT_BS1:
            for (uint32_t i = edited_pos + 1; i < s_count; i++) {
                if (!s_records[i].valid || s_records[i].voided) continue;
                if (s_records[i].sight == SIGHT_BS2) { SYNC(i); break; }
                if (s_records[i].sight == SIGHT_BS1) break;
            }
            {
                bool skipped_bs2 = false;
                for (int i = (int)edited_pos - 1; i >= 0; i--) {
                    if (!s_records[i].valid || s_records[i].voided) continue;
                    sight_type_t sv = s_records[i].sight;
                    if (!skipped_bs2 && sv == SIGHT_BS2) { skipped_bs2 = true; continue; }
                    if (sv == SIGHT_FS2) { SYNC(i); continue; }
                    if (sv == SIGHT_FS1) { SYNC(i); break; }
                    if (sv == SIGHT_BS1) break;
                }
            }
            break;

        /* ── BFFB BS2: sync BS1 backward, then prev-setup FS2+FS1 backward ── */
        case SIGHT_BS2: {
            uint32_t bs1_pos = UINT32_MAX;
            for (int i = (int)edited_pos - 1; i >= 0; i--) {
                if (!s_records[i].valid || s_records[i].voided) continue;
                if (s_records[i].sight == SIGHT_BS1) { SYNC(i); bs1_pos = (uint32_t)i; break; }
            }
            if (bs1_pos != UINT32_MAX) {
                bool skipped_bs2 = false;
                for (int i = (int)bs1_pos - 1; i >= 0; i--) {
                    if (!s_records[i].valid || s_records[i].voided) continue;
                    sight_type_t sv = s_records[i].sight;
                    if (!skipped_bs2 && sv == SIGHT_BS2) { skipped_bs2 = true; continue; }
                    if (sv == SIGHT_FS2) { SYNC(i); continue; }
                    if (sv == SIGHT_FS1) { SYNC(i); break; }
                    if (sv == SIGHT_BS1) break;
                }
            }
            break;
        }

        /* ── BFFB FS1: sync FS2 forward, then next-setup BS1+BS2 forward ── */
        case SIGHT_FS1:
            for (uint32_t i = edited_pos + 1; i < s_count; i++) {
                if (!s_records[i].valid || s_records[i].voided) continue;
                if (s_records[i].sight == SIGHT_FS2) { SYNC(i); break; }
                if (s_records[i].sight == SIGHT_FS1) break;
            }
            {
                bool past_bs2 = false, found_bs1 = false;
                for (uint32_t i = edited_pos + 1; i < s_count; i++) {
                    if (!s_records[i].valid || s_records[i].voided) continue;
                    sight_type_t sv = s_records[i].sight;
                    if (!past_bs2 && sv == SIGHT_BS2) { past_bs2 = true; continue; }
                    if (past_bs2 && !found_bs1 && sv == SIGHT_BS1) { SYNC(i); found_bs1 = true; continue; }
                    if (found_bs1 && sv == SIGHT_BS2) { SYNC(i); break; }
                    if (found_bs1 && sv == SIGHT_BS1) break;
                }
            }
            break;

        /* ── BFFB FS2: sync FS1 backward, then next-setup BS1+BS2 forward ── */
        case SIGHT_FS2:
            for (int i = (int)edited_pos - 1; i >= 0; i--) {
                if (!s_records[i].valid || s_records[i].voided) continue;
                if (s_records[i].sight == SIGHT_FS1) { SYNC(i); break; }
                if (s_records[i].sight == SIGHT_FS2) break;
            }
            {
                bool past_bs2 = false, found_bs1 = false;
                for (uint32_t i = edited_pos + 1; i < s_count; i++) {
                    if (!s_records[i].valid || s_records[i].voided) continue;
                    sight_type_t sv = s_records[i].sight;
                    if (!past_bs2 && sv == SIGHT_BS2) { past_bs2 = true; continue; }
                    if (past_bs2 && !found_bs1 && sv == SIGHT_BS1) { SYNC(i); found_bs1 = true; continue; }
                    if (found_bs1 && sv == SIGHT_BS2) { SYNC(i); break; }
                    if (found_bs1 && sv == SIGHT_BS1) break;
                }
            }
            break;

        /* ── BF BS: sync previous FS ─────────────────────────────────────── */
        case SIGHT_BS:
            for (int i = (int)edited_pos - 1; i >= 0; i--) {
                if (!s_records[i].valid || s_records[i].voided) continue;
                if (s_records[i].sight == SIGHT_FS) { SYNC(i); break; }
                if (s_records[i].sight == SIGHT_BS) break;
            }
            break;

        /* ── BF FS: sync next BS ─────────────────────────────────────────── */
        case SIGHT_FS:
            for (uint32_t i = edited_pos + 1; i < s_count; i++) {
                if (!s_records[i].valid || s_records[i].voided) continue;
                if (s_records[i].sight == SIGHT_BS) { SYNC(i); break; }
                if (s_records[i].sight == SIGHT_FS) break;
            }
            break;

        default: break;
        }
#undef SYNC
    }

    xSemaphoreGive(s_mtx);
    if (ret == ESP_OK) {
        storage_rewrite_csv(&s_job, s_records, s_count);
        ESP_LOGI(TAG, "Point #%lu name updated to [%s]", (unsigned long)index, name);
    }
    return ret;
}
void            job_lock(void)        { xSemaphoreTake(s_mtx, portMAX_DELAY); }
void            job_unlock(void)      { xSemaphoreGive(s_mtx); }
