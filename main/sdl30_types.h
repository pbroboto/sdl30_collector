/**
 * sdl30_types.h — Shared data structures for SDL30 Collector
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "config.h"

// ─── Sight type ───────────────────────────────────────────────────────────────
typedef enum {
    SIGHT_BS = 0,   // Backsight
    SIGHT_IS = 1,   // Intermediate sight
    SIGHT_FS = 2,   // Foresight
} sight_type_t;

static inline const char *sight_str(sight_type_t s) {
    switch (s) {
        case SIGHT_BS: return "BS";
        case SIGHT_IS: return "IS";
        case SIGHT_FS: return "FS";
        default:       return "??";
    }
}

static inline sight_type_t sight_from_str(const char *s) {
    if (!s) return SIGHT_IS;
    if (s[0] == 'B') return SIGHT_BS;
    if (s[0] == 'F') return SIGHT_FS;
    return SIGHT_IS;
}

// ─── Single measurement record ────────────────────────────────────────────────
typedef struct {
    uint32_t     index;       // 1-based point number
    sight_type_t sight;       // BS / IS / FS
    float        staff;       // staff reading in metres (signed)
    float        distance;    // horizontal distance in metres
    float        hi;          // Height of Instrument at this point
    float        rl;          // Reduced Level (calculated)
    bool         voided;      // soft-delete flag
    bool         valid;       // true if slot is in use
} record_t;

// ─── Job ──────────────────────────────────────────────────────────────────────
typedef struct {
    char     name[MAX_JOB_NAME];   // job name e.g. "RWY28L_2026"
    float    bench_rl;             // benchmark RL (starting elevation)
    float    current_hi;           // running Height of Instrument
    float    current_rl;           // running Reduced Level
    uint32_t point_count;          // total saved points
} job_t;
