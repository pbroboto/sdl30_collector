/**
 * storage/storage.c — SPIFFS persistence implementation
 */
#include "storage.h"
#include "../config.h"
#include "esp_spiffs.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

static const char *TAG = "STORAGE";

#define CSV_HEADER "Point,Sight,Staff(m),Distance(m),HI(m),RL(m),Status\n"

// ─── Init ─────────────────────────────────────────────────────────────────────
esp_err_t storage_init(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path              = SPIFFS_BASE,
        .partition_label        = NULL,
        .max_files              = SPIFFS_MAX_FILES,
        .format_if_mount_failed = true,
    };
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(err));
        return err;
    }
    size_t total = 0, used = 0;
    esp_spiffs_info(NULL, &total, &used);
    ESP_LOGI(TAG, "SPIFFS: %uKB total  %uKB used  %uKB free",
             (unsigned)(total/1024), (unsigned)(used/1024),
             (unsigned)((total-used)/1024));
    return ESP_OK;
}

void storage_info(size_t *total, size_t *used)
{
    esp_spiffs_info(NULL, total, used);
}

// ─── Metadata ─────────────────────────────────────────────────────────────────
esp_err_t storage_save_meta(const job_t *job)
{
    char path[64];
    snprintf(path, sizeof(path), "%s/%s.meta", SPIFFS_BASE, job->name);
    FILE *f = fopen(path, "w");
    if (!f) return ESP_FAIL;
    fprintf(f, "bench_rl=%.4f\npoint_count=%u\n",
            job->bench_rl, (unsigned)job->point_count);
    fclose(f);
    return ESP_OK;
}

esp_err_t storage_load_meta(job_t *job)
{
    char path[64];
    snprintf(path, sizeof(path), "%s/%s.meta", SPIFFS_BASE, job->name);
    FILE *f = fopen(path, "r");
    if (!f) return ESP_FAIL;
    char line[64];
    while (fgets(line, sizeof(line), f)) {
        float fv; unsigned uv;
        if (sscanf(line, "bench_rl=%f",    &fv) == 1) job->bench_rl    = fv;
        if (sscanf(line, "point_count=%u", &uv) == 1) job->point_count = uv;
    }
    fclose(f);
    return ESP_OK;
}

// ─── CSV append ───────────────────────────────────────────────────────────────
esp_err_t storage_append_record(const job_t *job, const record_t *r)
{
    char path[64];
    snprintf(path, sizeof(path), "%s/%s.csv", SPIFFS_BASE, job->name);
    FILE *f = fopen(path, "a");
    if (!f) { ESP_LOGE(TAG, "Cannot open: %s", path); return ESP_FAIL; }
    fseek(f, 0, SEEK_END);
    if (ftell(f) == 0) fprintf(f, CSV_HEADER);
    fprintf(f, "%lu,%s,%+.4f,%.3f,%.4f,%.4f,%s\n",
            (unsigned long)r->index, sight_str(r->sight),
            r->staff, r->distance, r->hi, r->rl,
            r->voided ? "VOID" : "OK");
    fclose(f);
    return ESP_OK;
}

// ─── CSV rewrite ──────────────────────────────────────────────────────────────
esp_err_t storage_rewrite_csv(const job_t *job,
                               const record_t *recs, uint32_t count)
{
    char path[64];
    snprintf(path, sizeof(path), "%s/%s.csv", SPIFFS_BASE, job->name);
    FILE *f = fopen(path, "w");
    if (!f) { ESP_LOGE(TAG, "Cannot open: %s", path); return ESP_FAIL; }
    fprintf(f, CSV_HEADER);
    for (uint32_t i = 0; i < count; i++) {
        const record_t *r = &recs[i];
        if (!r->valid) continue;
        fprintf(f, "%lu,%s,%+.4f,%.3f,%.4f,%.4f,%s\n",
                (unsigned long)r->index, sight_str(r->sight),
                r->staff, r->distance, r->hi, r->rl,
                r->voided ? "VOID" : "OK");
    }
    fclose(f);
    ESP_LOGI(TAG, "Rewrote %s (%u records)", path, (unsigned)count);
    return ESP_OK;
}

// ─── CSV load ─────────────────────────────────────────────────────────────────
uint32_t storage_load_records(const job_t *job,
                               record_t *recs, uint32_t max)
{
    char path[64];
    snprintf(path, sizeof(path), "%s/%s.csv", SPIFFS_BASE, job->name);
    FILE *f = fopen(path, "r");
    if (!f) return 0;

    uint32_t count = 0;
    char line[128];
    bool first = true;

    while (fgets(line, sizeof(line), f) && count < max) {
        if (first) { first = false; continue; }  // skip header
        if (line[0] == '\n' || line[0] == '\r') continue;

        record_t r = {0};
        char sight_s[4], status_s[8];
        if (sscanf(line, "%lu,%3[^,],%f,%f,%f,%f,%7s",
                   &r.index, sight_s,
                   &r.staff, &r.distance,
                   &r.hi, &r.rl, status_s) == 7) {
            r.sight  = sight_from_str(sight_s);
            r.voided = (status_s[0] == 'V');
            r.valid  = true;
            recs[count++] = r;
        }
    }
    fclose(f);
    ESP_LOGI(TAG, "Loaded %u records from %s", (unsigned)count, path);
    return count;
}

// ─── File management ──────────────────────────────────────────────────────────
int storage_list_jobs(char names[][MAX_JOB_NAME], int max_jobs)
{
    int count = 0;
    DIR *dir = opendir(SPIFFS_BASE);
    if (!dir) return 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && count < max_jobs) {
        char *dot = strrchr(ent->d_name, '.');
        // Use .meta as the canonical job marker
        if (dot && strcmp(dot, ".meta") == 0) {
            size_t nlen = dot - ent->d_name;
            if (nlen >= MAX_JOB_NAME) nlen = MAX_JOB_NAME - 1;
            strncpy(names[count], ent->d_name, nlen);
            names[count][nlen] = '\0';
            count++;
        }
    }
    closedir(dir);
    return count;
}

esp_err_t storage_delete_job(const char *jobname)
{
    char path[64];
    snprintf(path, sizeof(path), "%s/%s.csv",  SPIFFS_BASE, jobname);
    remove(path);
    snprintf(path, sizeof(path), "%s/%s.meta", SPIFFS_BASE, jobname);
    remove(path);
    ESP_LOGI(TAG, "Deleted job: %s", jobname);
    return ESP_OK;
}

size_t storage_csv_size(const char *jobname)
{
    char path[64];
    snprintf(path, sizeof(path), "%s/%s.csv", SPIFFS_BASE, jobname);
    struct stat st;
    return (stat(path, &st) == 0) ? st.st_size : 0;
}
