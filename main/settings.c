/**
 * settings.c — Survey settings persistence
 */
#include "settings.h"
#include "config.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "SETTINGS";
#define SETTINGS_FILE SPIFFS_BASE "/settings.cfg"

void settings_load(settings_t *s)
{
    // Fill defaults first
    s->obs_method      = DEF_OBS_METHOD;
    s->max_station_mm  = DEF_MAX_STATION_MM;
    s->max_dist_diff   = DEF_MAX_DIST_DIFF;
    s->max_cum_diff    = DEF_MAX_CUM_DIFF;
    s->min_sight_ht    = DEF_MIN_SIGHT_HT;
    s->max_sight_dist  = DEF_MAX_SIGHT_DIST;
    s->alert_on_exceed = DEF_ALERT_ON;
    s->block_on_exceed = DEF_BLOCK_ON;
    s->baud_rate       = SDL_BAUD;

    FILE *f = fopen(SETTINGS_FILE, "r");
    if (!f) {
        ESP_LOGI(TAG, "No settings file — using defaults");
        return;
    }
    char line[64];
    while (fgets(line, sizeof(line), f)) {
        float fv; int iv;
        if (sscanf(line, "obs_method=%d",      &iv) == 1) s->obs_method      = iv;
        if (sscanf(line, "max_station_mm=%f",  &fv) == 1) s->max_station_mm  = fv;
        if (sscanf(line, "max_dist_diff=%f",   &fv) == 1) s->max_dist_diff   = fv;
        if (sscanf(line, "max_cum_diff=%f",    &fv) == 1) s->max_cum_diff    = fv;
        if (sscanf(line, "min_sight_ht=%f",    &fv) == 1) s->min_sight_ht    = fv;
        if (sscanf(line, "max_sight_dist=%f",  &fv) == 1) s->max_sight_dist  = fv;
        if (sscanf(line, "alert_on_exceed=%d", &iv) == 1) s->alert_on_exceed = iv;
        if (sscanf(line, "block_on_exceed=%d", &iv) == 1) s->block_on_exceed = iv;
        if (sscanf(line, "baud_rate=%d",       &iv) == 1) s->baud_rate       = iv;
    }
    fclose(f);
    ESP_LOGI(TAG, "Settings loaded: method=%s baud=%d",
             obs_method_str(s->obs_method), s->baud_rate);
}

void settings_save(const settings_t *s)
{
    FILE *f = fopen(SETTINGS_FILE, "w");
    if (!f) { ESP_LOGE(TAG, "Cannot save settings"); return; }
    fprintf(f, "obs_method=%d\n",      s->obs_method);
    fprintf(f, "max_station_mm=%.2f\n",s->max_station_mm);
    fprintf(f, "max_dist_diff=%.2f\n", s->max_dist_diff);
    fprintf(f, "max_cum_diff=%.2f\n",  s->max_cum_diff);
    fprintf(f, "min_sight_ht=%.3f\n",  s->min_sight_ht);
    fprintf(f, "max_sight_dist=%.1f\n",s->max_sight_dist);
    fprintf(f, "alert_on_exceed=%d\n", s->alert_on_exceed ? 1 : 0);
    fprintf(f, "block_on_exceed=%d\n", s->block_on_exceed ? 1 : 0);
    fprintf(f, "baud_rate=%d\n",       s->baud_rate);
    fclose(f);
    ESP_LOGI(TAG, "Settings saved");
}
