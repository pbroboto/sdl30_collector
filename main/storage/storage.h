/**
 * storage/storage.h — SPIFFS file persistence
 *
 * File layout on SPIFFS:
 *   /spiffs/<jobname>.csv   — CSV data (master record, opened in Excel)
 *   /spiffs/<jobname>.meta  — job metadata (bench_rl, point_count)
 *
 * CSV format:
 *   Point,Sight,Staff(m),Distance(m),HI(m),RL(m),Status
 *   1,BS,+1.4230,023.456,101.4230,100.0000,OK
 *   2,IS,+1.1050,018.234,101.4230,100.3180,OK
 */
#pragma once
#include "../sdl30_types.h"
#include "esp_err.h"
#include <stddef.h>

/** Mount SPIFFS — call once at startup. */
esp_err_t storage_init(void);

/** Get total/used bytes. */
void storage_info(size_t *total, size_t *used);

/** Save job metadata (.meta file). */
esp_err_t storage_save_meta(const job_t *job);

/** Load job metadata from .meta file. */
esp_err_t storage_load_meta(job_t *job);

/** Append one record to CSV (fast path after each measurement). */
esp_err_t storage_append_record(const job_t *job, const record_t *r);

/** Rewrite entire CSV from RAM array (after delete/edit/recalc). */
esp_err_t storage_rewrite_csv(const job_t *job,
                               const record_t *recs, uint32_t count);

/** Load all records from CSV into RAM array. Returns count loaded. */
uint32_t storage_load_records(const job_t *job,
                               record_t *recs, uint32_t max);

/** List .csv job files. Returns count. */
int storage_list_jobs(char names[][MAX_JOB_NAME], int max_jobs);

/** Delete job CSV + meta files. */
esp_err_t storage_delete_job(const char *jobname);

/** Get CSV file size in bytes. */
size_t storage_csv_size(const char *jobname);
