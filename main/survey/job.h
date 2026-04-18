/**
 * survey/job.h — Job and measurement point management
 *
 * Manages the in-RAM record array and active job state.
 * Calls survey_recalc() after any edit or delete.
 * Calls storage_* functions to persist changes to SPIFFS.
 */
#pragma once
#include "../sdl30_types.h"
#include "esp_err.h"

/**
 * Initialise job module — call once at startup.
 * Creates the RAM record array and sets default job.
 */
esp_err_t job_init(void);

/**
 * Create a new job and make it active.
 * Clears RAM record array.
 *
 * @param name      job name (max MAX_JOB_NAME-1 chars)
 * @param bench_rl  starting benchmark RL in metres
 */
esp_err_t job_new(const char *name, float bench_rl);

/**
 * Load an existing job from SPIFFS and make it active.
 * Reads CSV, restores RAM record array and running HI/RL.
 */
esp_err_t job_select(const char *name);

/**
 * Delete a job from SPIFFS (CSV + meta files).
 * If deleting the active job, resets to blank state.
 */
esp_err_t job_delete(const char *name);

/**
 * Set benchmark RL on the active job.
 * Triggers full RL recalculation and CSV rewrite.
 */
esp_err_t job_set_bench(float bench_rl);

/**
 * Add a new measurement point to the active job.
 * Calculates HI/RL, appends to RAM array and SPIFFS CSV.
 *
 * @param sight     BS / IS / FS
 * @param staff     staff reading in metres
 * @param distance  distance in metres
 */
esp_err_t job_add_point(sight_type_t sight, float staff, float distance);

/**
 * Delete a record by index (1-based).
 * Removes from RAM array, renumbers remaining points,
 * triggers full RL recalculation, rewrites SPIFFS CSV.
 */
esp_err_t job_delete_point(uint32_t index);

/**
 * Change the sight type of a record (BS/IS/FS).
 * Triggers full RL recalculation and CSV rewrite.
 */
esp_err_t job_edit_sight(uint32_t index, sight_type_t new_sight);
esp_err_t job_edit_name(uint32_t index, const char *name);

/**
 * Get pointer to active job info (read-only).
 */
const job_t *job_get_info(void);

/**
 * Get pointer to RAM record array (read-only).
 */
const record_t *job_get_records(void);

/**
 * Get current record count.
 */
uint32_t job_get_count(void);
uint32_t job_get_fs_count(void);
uint32_t job_get_bs2_count(void);

/**
 * Take/give the data mutex — used by web server
 * when iterating records to build JSON response.
 */
void job_lock(void);
void job_unlock(void);
