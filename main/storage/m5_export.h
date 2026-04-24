/**
 * storage/m5_export.h — Trimble DiNi M5 format export
 *
 * Generates M5 on-demand from the existing record_t array.
 * No changes to record_t, storage.c, or survey.c required.
 *
 * M5 line: 121 bytes exactly (119 data + CR + LF)
 * One call to m5_export_job() writes the complete file.
 *
 * Setup number and method are inferred from sight type:
 *   SIGHT_BS / SIGHT_FS / SIGHT_IS          → BF method
 *   SIGHT_BS1/BS2 / SIGHT_FS1/FS2           → BFFB method
 *   New setup starts on each SIGHT_BS/BS1
 *
 * HI and RL are taken directly from record_t (already computed
 * by survey_recalc — no recomputation needed).
 */
#pragma once
#include "../sdl30_types.h"
#include <stdio.h>
#include <stdint.h>

/**
 * Write a complete M5 file for one job.
 *
 * @param f         open FILE* to write to (caller opens/closes)
 * @param job_name  used in Start-Line comment field
 * @param recs      record array from job_get_records()
 * @param count     record count from job_get_count()
 * @param bench_rl  opening BM RL from job_get_info()->bench_rl
 *
 * @return  number of M5 address lines written, -1 on error
 */
int m5_export_job(FILE *f,
                  const char *job_name,
                  const record_t *recs,
                  uint32_t count,
                  float bench_rl);
