/**
 * storage/m5_export.c — Trimble DiNi M5 format export
 *
 * Confirmed field structure from real DiNi files:
 *
 * KD1 measurement:
 *   PNo(%8.8s) + 14sp + Sno(1) + 3sp + Zno(1) = 27
 *   Sno = number of measurements (always 1 for SDL30)
 *   Zno = line/section number (always 1 for SDL30 Collector)
 *
 * KD1 Z/Sh record (computed elevation):
 *   PNo(%8.8s) + 18sp + Zno(1) = 27
 *
 * KD2 closing:
 *   PNo(%8.8s) + 7sp + setup_count(%2d) + 9sp + Zno(1) = 27
 *
 * TO text record:
 *   %-10.10s + 9spaces + %-4.4s(method) + %4d(zno) = 27
 *
 * Z record placement:
 *   BF:   written after FS  (Rf -> Z)
 *   BFFB: written after BS2 (Rb -> Z), using mean RL
 *         mean_RL = (prev_rl + (BS1+BS2)/2) - (FS1+FS2)/2
 *
 * Mixed BF/BFFB: Cont-Line inserted on method change between setups.
 */
#include "m5_export.h"
#include "storage.h"
#include "../config.h"
#include "esp_log.h"
#include <string.h>
#include <math.h>

static const char *TAG = "M5";

#define M5_EMPTY  "                      "  /* 22 spaces = one empty data block */
#define M5_M      "m   "                    /* 4-char metre unit                */
#define M5_SNO    1                         /* SDL30: always 1 measurement      */
#define M5_ZNO    1                         /* SDL30 Collector: always line 1   */

/* ── Info block builders ─────────────────────────────────────────────────── */

/* KD1 measurement: PNo(8)+14sp+Sno(1)+3sp+Zno(1) = 27 */
static void make_info_meas(char buf[28], const char *name)
{
    snprintf(buf, 28, "%8.8s              %1d   %1d", name, M5_SNO, M5_ZNO);
}

/* KD1 Z/Sh record: PNo(8)+18sp+Zno(1) = 27 */
static void make_info_z(char buf[28], const char *name)
{
    snprintf(buf, 28, "%8.8s%19d", name, M5_ZNO);
}

/* KD2 closing: PNo(8)+7sp+setups(2)+9sp+Zno(1) = 27 */
static void make_info_kd2(char buf[28], const char *name, int setup_count)
{
    snprintf(buf, 28, "%8.8s       %2d         1", name, setup_count);
}

/* TO text: %-10.10s + 9sp + %-4.4s(method) + %4d(zno) = 27 */
static void make_info_to(char buf[28], const char *keyword, const char *method)
{
    snprintf(buf, 28, "%-10.10s       %-4.4s     %1d",
             keyword,
             method ? method : "",
             M5_ZNO);
}

/* TO note: comment (23 chars) + zno (4) = 27 */
static void make_info_note(char buf[28], const char *comment)
{
    snprintf(buf, 28, "%-23.23s%4d", comment, M5_ZNO);
}

/* ── Write one 121-byte M5 line ──────────────────────────────────────────── */
static void write_line(FILE *f, uint32_t *addr,
                       const char *type2, const char *info,
                       const char *t3, float v3,
                       const char *t4, float v4,
                       const char *t5, float v5)
{
    char b3[23], b4[23], b5[23];

    if (t3) snprintf(b3, sizeof(b3), "%-2s %14.5f %-4s", t3, v3, M5_M);
    else    strncpy(b3, M5_EMPTY, sizeof(b3));

    if (t4) snprintf(b4, sizeof(b4), "%-2s %14.5f %-4s", t4, v4, M5_M);
    else    strncpy(b4, M5_EMPTY, sizeof(b4));

    if (t5) snprintf(b5, sizeof(b5), "%-2s %14.5f %-4s", t5, v5, M5_M);
    else    strncpy(b5, M5_EMPTY, sizeof(b5));

    fprintf(f, "For M5|Adr%6lu|%-3s %-27s|%s|%s|%s| \r\n",
            (unsigned long)(*addr)++,
            type2, info, b3, b4, b5);
}

/* ── Helpers ─────────────────────────────────────────────────────────────── */
static inline int is_bffb_sight(sight_type_t s)
{
    return (s == SIGHT_BS1 || s == SIGHT_FS1 ||
            s == SIGHT_FS2 || s == SIGHT_BS2);
}

static inline const char *infer_method(sight_type_t s)
{
    return is_bffb_sight(s) ? "BFFB" : "BF";
}

static inline int opens_setup(sight_type_t s)
{
    return (s == SIGHT_BS || s == SIGHT_BS1);
}


/* ── Find note for a specific record ────────────────────────────────────── */
static int find_note(char pnames[][MAX_POINT_NAME],
                     char sights[][4],
                     uint32_t *setup_nos,
                     char comments[][MAX_COMMENT_LEN + 1],
                     int note_count,
                     const char *name, const char *sight, uint32_t setup_no,
                     char *comment_out)
{
    for (int i = 0; i < note_count; i++) {
        if (strcmp(pnames[i], name)  == 0 &&
            strcmp(sights[i], sight) == 0 &&
            setup_nos[i] == setup_no) {
            strncpy(comment_out, comments[i], MAX_COMMENT_LEN);
            comment_out[MAX_COMMENT_LEN] = '\0';
            return 1;
        }
    }
    return 0;
}

/* ── Main export ─────────────────────────────────────────────────────────── */
int m5_export_job(FILE *f,
                  const char *job_name,
                  const record_t *recs,
                  uint32_t count,
                  float bench_rl)
{
    if (!f || !recs || count == 0) return -1;

    /* Load notes once (static: called non-reentrant under job_lock) */
    static char     s_nnames[MAX_NOTES][MAX_POINT_NAME];
    static char     s_nsights[MAX_NOTES][4];
    static uint32_t s_nsetups[MAX_NOTES];
    static char     s_ncomments[MAX_NOTES][MAX_COMMENT_LEN + 1];
    int note_count = storage_load_notes(job_name,
                                        s_nnames, s_nsights,
                                        s_nsetups, s_ncomments, MAX_NOTES);
    char note_buf[MAX_COMMENT_LEN + 1];

    uint32_t addr     = 1;
    char     info[28];
    float    db       = 0.0f;
    float    df       = 0.0f;
    uint32_t setup_no = 0;

    char cur_method[5]  = "BF";
    char line_method[5] = "BF";
    int  first_setup    = 1;
    uint32_t last_setup_no = 0; (void)last_setup_no;

    /* BFFB per-setup accumulator */
    float       bffb_prev_rl = 0.0f;
    float       bffb_bs1     = 0.0f;
    float       bffb_fs1     = 0.0f;
    float       bffb_fs2     = 0.0f;
    const char *bffb_tp      = "TP";

    /* Determine first method */
    for (uint32_t i = 0; i < count; i++) {
        if (recs[i].valid && !recs[i].voided) {
            strncpy(line_method, infer_method(recs[i].sight),
                    sizeof(line_method) - 1);
            strncpy(cur_method, line_method, sizeof(cur_method) - 1);
            break;
        }
    }

    ESP_LOGI(TAG, "M5 export: job=%s  count=%lu  bench=%.4f  method=%s",
             job_name, (unsigned long)count, bench_rl, line_method);

    /* TO: Job name — filename with .dat extension, before Start-Line */
    char job_dat[MAX_JOB_NAME + 5];
    snprintf(job_dat, sizeof(job_dat), "%s.dat", job_name);
    snprintf(info, sizeof(info), "%-27.27s", job_dat);
    write_line(f, &addr, "TO ", info, NULL,0, NULL,0, NULL,0);

    /* TO: Start-Line */
    make_info_to(info, "Start-Line", line_method);
    write_line(f, &addr, "TO ", info, NULL,0, NULL,0, NULL,0);

    /* KD1: Opening BM reference height */
    const char *bm_name = "BM001";
    for (uint32_t i = 0; i < count; i++) {
        if (recs[i].valid && !recs[i].voided && opens_setup(recs[i].sight)) {
            bm_name = recs[i].name;
            break;
        }
    }
    make_info_z(info, bm_name);
    write_line(f, &addr, "KD1", info, NULL,0, NULL,0, "Z ", bench_rl);

/* Emit a note TO record after a measurement if one exists */
#define EMIT_NOTE(name_, sight_, setup_) \
    if (find_note(s_nnames, s_nsights, s_nsetups, s_ncomments, note_count, \
                  (name_), sight_str(sight_), (setup_), note_buf)) {       \
        make_info_note(info, note_buf);                                    \
        write_line(f, &addr, "TO ", info, NULL,0, NULL,0, NULL,0);        \
    }

    /* Walk all records */
    for (uint32_t i = 0; i < count; i++) {
        const record_t *r = &recs[i];
        if (!r->valid || r->voided) continue;

        sight_type_t s = r->sight;
        const char  *m = infer_method(s);

        /* Detect new setup using r->setup_no (stored in record) */
        if (opens_setup(s)) {
            if (!first_setup && strcmp(m, cur_method) != 0) {
                /* Method changed — insert Cont-Line */
                make_info_to(info, "Cont-Line", NULL);
                write_line(f, &addr, "TO ", info, NULL,0, NULL,0, NULL,0);
                ESP_LOGI(TAG, "Cont-Line: setup %lu %s->%s",
                         (unsigned long)r->setup_no, cur_method, m);
            }
            setup_no = r->setup_no;   /* use stored value directly */
            strncpy(cur_method, m, sizeof(cur_method) - 1);
            first_setup = 0;
            bffb_prev_rl = bffb_bs1 = bffb_fs1 = bffb_fs2 = 0.0f;
            bffb_tp = "TP";
        }

        make_info_meas(info, r->name);

        switch (s) {

        /* BF backsight: Rb + HD */
        case SIGHT_BS:
            db += r->distance;
            write_line(f, &addr, "KD1", info,
                       "Rb", r->staff, "HD", r->distance, NULL,0);
            EMIT_NOTE(r->name, s, r->setup_no);
            break;

        /* BF foresight: Rf + HD, then Z */
        case SIGHT_FS:
            df += r->distance;
            write_line(f, &addr, "KD1", info,
                       "Rf", r->staff, "HD", r->distance, NULL,0);
            make_info_z(info, r->name);
            write_line(f, &addr, "KD1", info,
                       NULL,0, NULL,0, "Z ", r->rl);
            EMIT_NOTE(r->name, s, r->setup_no);
            break;

        /* Intermediate sight: Rz + HD + Z */
        case SIGHT_IS:
            write_line(f, &addr, "KD1", info,
                       "Rz", r->staff, "HD", r->distance, "Z ", r->rl);
            EMIT_NOTE(r->name, s, r->setup_no);
            break;

        /* BFFB BS1: Rb + HD, save state */
        case SIGHT_BS1:
            db += r->distance;
            bffb_prev_rl = r->rl;    /* RL at BM point before this setup */
            bffb_bs1     = r->staff;
            write_line(f, &addr, "KD1", info,
                       "Rb", r->staff, "HD", r->distance, NULL,0);
            EMIT_NOTE(r->name, s, r->setup_no);
            break;

        /* BFFB FS1: Rf + HD, save state */
        case SIGHT_FS1:
            df += r->distance;
            bffb_fs1 = r->staff;
            bffb_tp  = r->name;
            write_line(f, &addr, "KD1", info,
                       "Rf", r->staff, "HD", r->distance, NULL,0);
            EMIT_NOTE(r->name, s, r->setup_no);
            break;

        /* BFFB FS2: Rf + HD only — NO Z here */
        case SIGHT_FS2:
            df += r->distance;
            bffb_fs2 = r->staff;
            write_line(f, &addr, "KD1", info,
                       "Rf", r->staff, "HD", r->distance, NULL,0);
            EMIT_NOTE(r->name, s, r->setup_no);
            break;

        /* BFFB BS2: Rb + HD, then Z with mean RL */
        case SIGHT_BS2: {
            db += r->distance;
            write_line(f, &addr, "KD1", info,
                       "Rb", r->staff, "HD", r->distance, NULL,0);

            /*
             * Mean TP RL:
             *   mean_HI = prev_rl + (BS1 + BS2) / 2
             *   tp_rl   = mean_HI - (FS1 + FS2) / 2
             */
            float bs_mean = (bffb_bs1 + r->staff) / 2.0f;
            float fs_mean = (bffb_fs1 + bffb_fs2) / 2.0f;
            float tp_rl   = bffb_prev_rl + bs_mean - fs_mean;

            make_info_z(info, bffb_tp);
            write_line(f, &addr, "KD1", info,
                       NULL,0, NULL,0, "Z ", tp_rl);

            ESP_LOGI(TAG,
                "BFFB Z: prev=%.4f bs_mean=%.4f fs_mean=%.4f tp_rl=%.4f",
                bffb_prev_rl, bs_mean, fs_mean, tp_rl);
            EMIT_NOTE(r->name, s, r->setup_no);
            break;
        }

        default: break;
        }
    }
#undef EMIT_NOTE

    /* KD2: closing record
     * Closing RL = last Z value written.
     * For BF: last FS r->rl.
     * For BFFB: recompute mean from last BS2 setup. */
    float      closing_rl   = bench_rl;
    const char *closing_name = bm_name;

    for (int i = (int)count - 1; i >= 0; i--) {
        if (!recs[i].valid || recs[i].voided) continue;

        if (recs[i].sight == SIGHT_FS) {
            closing_rl   = recs[i].rl;
            closing_name = recs[i].name;
            break;
        }
        if (recs[i].sight == SIGHT_BS2) {
            float bs1s = 0, fs1s = 0, fs2s = 0, prev = 0;
            const char *tp = "TP";
            for (int j = i - 1; j >= 0; j--) {
                if (!recs[j].valid || recs[j].voided) continue;
                if (recs[j].sight == SIGHT_FS2 && fs2s == 0.0f) {
                    fs2s = recs[j].staff;
                    tp   = recs[j].name;
                } else if (recs[j].sight == SIGHT_FS1 && fs1s == 0.0f) {
                    fs1s = recs[j].staff;
                } else if (recs[j].sight == SIGHT_BS1 && bs1s == 0.0f) {
                    bs1s = recs[j].staff;
                    prev = recs[j].rl;
                    break;
                }
            }
            float bs_mean  = (bs1s + recs[i].staff) / 2.0f;
            float fs_mean  = (fs1s + fs2s) / 2.0f;
            closing_rl     = prev + bs_mean - fs_mean;
            closing_name   = tp;
            break;
        }
    }

    /* Sh/dz/Z closing record — height diff, closure error, known RL. */
    float sh = closing_rl - bench_rl;
    float dz = bench_rl - closing_rl;
    make_info_z(info, closing_name);
    write_line(f, &addr, "KD1", info, "Sh", sh, "dz", dz, "Z ", bench_rl);

    make_info_kd2(info, closing_name, (int)setup_no);  /* setup_no = last r->setup_no */
    write_line(f, &addr, "KD2", info,
               "Db", db, "Df", df, "Z ", closing_rl);

    /* TO: End-Line */
    make_info_to(info, "End-Line", NULL);
    write_line(f, &addr, "TO ", info, NULL,0, NULL,0, NULL,0);

    int total = (int)(addr - 1);
    ESP_LOGI(TAG, "M5 done: %d lines  setups=%lu  Db=%.3f  Df=%.3f  close=%.4f",
             total, (unsigned long)setup_no, db, df, closing_rl);
    return total;
}
