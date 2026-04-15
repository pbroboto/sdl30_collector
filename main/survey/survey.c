/**
 * survey/survey.c — Leveling arithmetic implementation
 *
 * Supports BF (basic) and double-reading methods (BFFB/BFBF/BBFF).
 *
 * Double-reading record sequence per setup:
 *   BS1 → FS1 → FS2 → BS2  (BFFB)
 *   BS1 → FS1 → BS2 → FS2  (BFBF)
 *   BS1 → BS2 → FS1 → FS2  (BBFF)
 *
 * RL assignments:
 *   BS1: HI = prev_RL + BS1;  RL = prev_RL (instrument setup)
 *   FS1: RL = HI - FS1        (first foresight estimate)
 *   FS2: RL = HI - FS2        (second foresight estimate)
 *   BS2: RL = prev_BM + BS1 - BS2  (sinking check)
 *
 * The mean RL (carried forward) is stored in the last record of
 * each setup (BS2 or FS2 depending on method) as r->rl after
 * the complete setup is processed.
 *
 * Arithmetic (Records/Report tabs):
 *   Σ BS = (Σ BS1 + Σ BS2) / 2
 *   Σ FS = (Σ FS1 + Σ FS2) / 2
 *   ΔElev = Σ BS - Σ FS
 */
#include "survey.h"
#include <math.h>

// Returns true if sight is a double-reading backsight
static bool is_bs_dblr(sight_type_t s) {
    return s == SIGHT_BS1 || s == SIGHT_BS2;
}

// Returns true if sight is a double-reading foresight
static bool is_fs_dblr(sight_type_t s) {
    return s == SIGHT_FS1 || s == SIGHT_FS2;
}

// Returns true if sight starts a new instrument setup
static bool is_setup_start(sight_type_t s) {
    return s == SIGHT_BS || s == SIGHT_BS1;
}

void survey_recalc(record_t *recs, uint32_t count, float bench_rl)
{
    float hi      = bench_rl;
    float rl      = bench_rl;    // current running RL
    float prev_bm = bench_rl;    // RL at last BS1 position

    // For double-reading: track BS1 staff to compute BS2 sinking RL
    float last_bs1_staff = 0.0f;

    for (uint32_t i = 0; i < count; i++) {
        record_t *r = &recs[i];
        if (!r->valid) continue;
        if (r->voided) { r->hi = 0.0f; r->rl = 0.0f; continue; }

        switch (r->sight) {

            // ── BF method ──────────────────────────────────────────────
            case SIGHT_BS:
                hi    = rl + r->staff;
                r->hi = hi;
                r->rl = rl;
                break;

            case SIGHT_IS:
            case SIGHT_FS:
                rl    = hi - r->staff;
                r->hi = hi;
                r->rl = rl;
                break;

            // ── Double-reading method ───────────────────────────────────
            case SIGHT_BS1:
                // New instrument setup — HI from previous RL
                prev_bm       = rl;
                last_bs1_staff = r->staff;
                hi    = rl + r->staff;
                r->hi = hi;
                r->rl = rl;   // BS1 shows the RL before this setup
                break;

            case SIGHT_FS1:
                // First foresight estimate
                rl    = hi - r->staff;
                r->hi = hi;
                r->rl = rl;
                break;

            case SIGHT_FS2:
                // Second foresight estimate
                r->hi = hi;
                r->rl = hi - r->staff;
                // rl stays as FS1 value until BS2 updates mean
                break;

            case SIGHT_BS2: {
                // Sinking check RL: prev_BM + BS1 - BS2
                float sinking_rl = prev_bm + last_bs1_staff - r->staff;
                r->hi = hi;
                r->rl = sinking_rl;

                // Find matching FS1 and FS2 to compute mean RL
                // Scan back to find FS2 then FS1
                float fs1 = 0.0f, fs2 = 0.0f;
                int found_fs2 = 0, found_fs1 = 0;
                for (int j = (int)i - 1; j >= 0; j--) {
                    if (!recs[j].valid || recs[j].voided) continue;
                    if (!found_fs2 && (recs[j].sight==SIGHT_FS2 || recs[j].sight==SIGHT_FS)) {
                        fs2 = recs[j].staff; found_fs2 = 1;
                    } else if (!found_fs1 && found_fs2 &&
                               (recs[j].sight==SIGHT_FS1 || recs[j].sight==SIGHT_FS)) {
                        fs1 = recs[j].staff; found_fs1 = 1;
                    }
                    if (found_fs1 && found_fs2) break;
                }

                if (found_fs1 && found_fs2) {
                    float bs_mean  = (last_bs1_staff + r->staff) / 2.0f;
                    float fs_mean  = (fs1 + fs2) / 2.0f;
                    float dh_mean  = bs_mean - fs_mean;
                    rl = prev_bm + dh_mean;   // mean RL carries forward
                } else {
                    // Fallback — use FS1 RL
                    for (int j = (int)i - 1; j >= 0; j--) {
                        if (recs[j].valid && !recs[j].voided &&
                            (recs[j].sight==SIGHT_FS1||recs[j].sight==SIGHT_FS)){
                            rl = recs[j].rl; break;
                        }
                    }
                }
                break;
            }

            default: break;
        }
    }
}

float survey_misclose(const record_t *recs, uint32_t count,
                      float bench_rl, float closing_rl,
                      float *sum_bs, float *sum_fs)
{
    float s_bs1 = 0.0f, s_bs2 = 0.0f;
    float s_fs1 = 0.0f, s_fs2 = 0.0f;
    float s_bs  = 0.0f, s_fs  = 0.0f;
    float last_rl = bench_rl;
    int   has_dblr = 0;

    for (uint32_t i = 0; i < count; i++) {
        const record_t *r = &recs[i];
        if (!r->valid || r->voided) continue;

        switch (r->sight) {
            case SIGHT_BS:  s_bs  += r->staff; break;
            case SIGHT_FS:  s_fs  += r->staff; break;
            case SIGHT_BS1: s_bs1 += r->staff; has_dblr = 1; break;
            case SIGHT_BS2: s_bs2 += r->staff; break;
            case SIGHT_FS1: s_fs1 += r->staff; break;
            case SIGHT_FS2: s_fs2 += r->staff; break;
            default: break;
        }
    }

    // Combine BF and double-reading sums
    // Σ BS = BF_BS + (BS1+BS2)/2
    // Σ FS = BF_FS + (FS1+FS2)/2
    float total_bs = s_bs + (s_bs1 + s_bs2) / 2.0f;
    float total_fs = s_fs + (s_fs1 + s_fs2) / 2.0f;

    if (sum_bs) *sum_bs = total_bs;
    if (sum_fs) *sum_fs = total_fs;

    // Last valid RL from FS/FS2/BS2
    for (int i = (int)count - 1; i >= 0; i--) {
        const record_t *r = &recs[i];
        if (!r->valid || r->voided) continue;
        if (r->sight==SIGHT_FS || r->sight==SIGHT_BS2) {
            last_rl = r->rl;
            break;
        }
    }

    (void)has_dblr;
    return last_rl - closing_rl;
}
