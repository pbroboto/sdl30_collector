/**
 * settings.h — Survey settings structure
 */
#pragma once
#include <stdbool.h>
#include "config.h"

typedef struct {
    int   obs_method;       // 0=BF 1=BFFB 2=BFBF 3=BBFF
    float max_station_mm;   // max station difference (mm)
    float max_dist_diff;    // max distance difference per setup (m)
    float max_cum_diff;     // max cumulative distance difference (m)
    float min_sight_ht;     // minimum sighting height (m)
    float max_sight_dist;   // maximum sighting distance (m)
    bool  alert_on_exceed;  // warn when limit exceeded
    bool  block_on_exceed;  // block save when limit exceeded
    int   baud_rate;        // 1200 or 2400
} settings_t;

static const char *obs_method_str(int m) {
    switch (m) {
        case 0: return "BF";
        case 1: return "BFFB";
        case 2: return "BFBF";
        case 3: return "BBFF";
        default: return "BF";
    }
}

/** Load settings from SPIFFS. Fills defaults if file missing. */
void settings_load(settings_t *s);

/** Save settings to SPIFFS. */
void settings_save(const settings_t *s);
