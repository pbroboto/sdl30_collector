/**
 * survey/survey.h — Pure leveling arithmetic
 *
 * HI (Height of Instrument) method:
 *   BS:  HI  = RL_prev + staff_BS
 *   IS:  RL  = HI - staff_IS
 *   FS:  RL  = HI - staff_FS  (next BS starts new HI)
 *
 * These functions have NO hardware dependency — pure math only.
 * Easy to unit test on any platform.
 */
#pragma once
#include "../sdl30_types.h"

/**
 * Recalculate HI and RL for every non-voided record
 * starting from bench_rl forward.
 *
 * Call this after any edit, delete, or benchmark change.
 */
// Returns the final carry-forward RL (mean RL after last complete setup).
// Use this to seed current_rl when loading a job from storage.
float survey_recalc(record_t *recs, uint32_t count, float bench_rl);

/**
 * Calculate misclose for a level run.
 *
 * @param recs        record array
 * @param count       number of records
 * @param bench_rl    opening benchmark RL
 * @param closing_rl  closing benchmark RL (same as bench_rl for loop)
 * @param sum_bs      output: sum of all BS readings
 * @param sum_fs      output: sum of all FS readings
 * @return            misclose = last_rl - closing_rl
 */
float survey_misclose(const record_t *recs, uint32_t count,
                      float bench_rl, float closing_rl,
                      float *sum_bs, float *sum_fs);
