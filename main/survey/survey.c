/**
 * survey/survey.c — Leveling arithmetic implementation
 */
#include "survey.h"

void survey_recalc(record_t *recs, uint32_t count, float bench_rl)
{
    float hi = bench_rl;
    float rl = bench_rl;

    for (uint32_t i = 0; i < count; i++) {
        record_t *r = &recs[i];
        if (!r->valid) continue;

        if (r->voided) {
            r->hi = 0.0f;
            r->rl = 0.0f;
            continue;
        }

        switch (r->sight) {
            case SIGHT_BS:
                hi   = rl + r->staff;
                r->hi = hi;
                r->rl = rl;
                break;
            case SIGHT_IS:
            case SIGHT_FS:
                rl    = hi - r->staff;
                r->hi = hi;
                r->rl = rl;
                break;
        }
    }
}

float survey_misclose(const record_t *recs, uint32_t count,
                      float bench_rl, float closing_rl,
                      float *sum_bs, float *sum_fs)
{
    float s_bs = 0.0f, s_fs = 0.0f;
    float last_rl = bench_rl;

    for (uint32_t i = 0; i < count; i++) {
        const record_t *r = &recs[i];
        if (!r->valid || r->voided) continue;
        if (r->sight == SIGHT_BS) s_bs += r->staff;
        if (r->sight == SIGHT_FS) s_fs += r->staff;
    }

    // Last valid FS is the closing RL
    for (int i = (int)count - 1; i >= 0; i--) {
        const record_t *r = &recs[i];
        if (r->valid && !r->voided && r->sight == SIGHT_FS) {
            last_rl = r->rl;
            break;
        }
    }

    if (sum_bs) *sum_bs = s_bs;
    if (sum_fs) *sum_fs = s_fs;

    return last_rl - closing_rl;
}
