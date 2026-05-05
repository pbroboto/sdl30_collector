# SDL30 Collector V2 — Development Log (ESP32-S3-N16)

## Overview

ESP32-S3-based wireless data collector for Sokkia SDL30 digital level, replacing the obsolete SDR33 data collector (original cost ~$1,000+ USD) with a DIY solution under ฿1,000 THB.

**Hardware:** ESP32-S3-N16R8 (16MB flash, 8MB PSRAM)
**Target device:** Sokkia SDL30 SN:001786, ROM 1112 (older model without name field support)
**Branch:** `v2-esp32s3`

**Mission:** Add modern features (point names, digital workflows, BFFB method) that newer SDL30 firmware versions have, but this older unit lacks.

> V1 (ESP32-WROOM-32) history: see `SDL30_Collector_V1_DEVELOPMENT.md`

---

## Hardware

### V1 — ESP32-WROOM-32 (reference only)

| Component | Part | Price (THB) |
|-----------|------|-------------|
| MCU | ESP32-WROOM-32 DevKit | 80-120 |
| RS-232 level shifter | SP3232EEN module | 40 |
| Cable | USB-A to Hirose 6-pin | 200 |
| Battery | 18650 3200mAh | 150 |
| Battery board | PL4506+MT3608 all-in-one | 80 |
| Enclosure, wiring | ABS box 120×97×40mm | 100 |
| **TOTAL** | | **~650-700 THB** |

### V2 — ESP32-S3-N16 (current hardware)

Same BOM, MCU upgraded to ESP32-S3-N16R8:
- 16MB flash → ~13MB SPIFFS → ~295,000 records possible
- 8MB PSRAM → larger web UI without size concerns
- Dual-core LX7 @ 240MHz → faster UI response
- USB-OTG support (direct PC connection possible)
- Same GPIO 16/17 UART pins — no wiring changes needed

### Wiring (both versions)

```
SDL30 (Hirose 6-pin) → USB-A cable → RS-232 breakout:
  Pin1 Black = GND
  Pin3 White = TXD (SDL30 → host)
  Pin4 Green = RXD (host → SDL30)
  Pin5 Red   = NC (+5V on some pinouts)

SP3232EEN module:
  RS232 TX/RX ← connects to Hirose cable wires
  TTL TXD (White) → ESP32 GPIO 16 (SDL_GPIO_RX)
  TTL RXD (Green) → ESP32 GPIO 17 (SDL_GPIO_TX)
  VCC → ESP32 3.3V
  GND → ESP32 GND
```

### config.h

```c
#define SDL_UART_NUM        UART_NUM_2
#define SDL_GPIO_RX         GPIO_NUM_16   // SP3232EEN TTL TXD (white wire)
#define SDL_GPIO_TX         GPIO_NUM_17   // SP3232EEN TTL RXD (green wire)
#define SDL_BAUD            2400          // confirmed: 2400 works with SDL30
#define SDL_TIMEOUT_MS      15000         // hot weather + 60m distance safety margin
```

---

## SDL30 Protocol

### Commands (Simple RS-232 ASCII)

| Command | Purpose | Response Example |
|---------|---------|------------------|
| `LM\r` | Measure | `LM 0.7890,1.88\r\n` |
| `LA\r` | Info | `LA SDL30,001786,1112\r\n` |
| `LB\r` | Parameters | `LB 0,0\r\n` |
| `LT\r` | Stop | (no response) |
| `LXa\r` | Single mode | ACK (0x06) |
| `L/B 0,0\r` | 0.0001m resolution | ACK |

### LM Response Format

**Manual says:** `LM _-9.9999,999.999\r\n`
**SDL30 actually sends:** `LM 0.7890,1.88\r\n`

Differences from manual:
- No `+` sign on positive values
- No leading zeros on distance
- No space after comma
- One space after `LM` always

**Lesson:** Always test with real hardware. Manuals are starting points, not scripture.

---

## Software Architecture

### ESP-IDF v5.4 — Modular C Code

```
main/
├── main.c                  # App entry, SDL30 connection check
├── config.h                # Hardware pins, timeouts, AP credentials
├── sdl30_types.h           # Common types (record_t, sight_type_t)
├── sdl30/
│   ├── sdl30.c/h           # UART protocol, adaptive timeout
├── survey/
│   ├── job.c/h             # Job management, auto-naming, HI/RL calc
│   ├── survey.c/h          # Pure leveling arithmetic (no hardware deps)
├── storage/
│   ├── storage.c/h         # SPIFFS, CSV append/rewrite, meta files
│   ├── m5_export.c/h       # Zeiss/DiNi M5 format export
└── web/
    ├── web_server.c/h      # REST API endpoints
    └── web_ui.h            # Single-page web app (HTML/CSS/JS in C string)
```

### Features

**Observation Methods:**
- **BF** (Basic/Fast) — single BS + FS per setup
- **BFFB** (3rd order) — BS1, FS1, FS2, BS2 with sinking check

**Point Name System:**
- Auto-generated: BM001 (first BS), TP001/TP002 leapfrog (each new FS)
- BS2 copies from BS1 of current setup
- FS2 copies from FS1 of current setup
- Manual edit via web UI (tap name cell in Records tab → prompt)
- `name[24]` field in `record_t` struct

**Job Management:**
- Multiple jobs with meta file (bench_rl, point_count)
- CSV auto-saves each record immediately on measurement
- All HI/RL recalculated from staff readings on every job load
- Restore last active job on reboot

**Web UI (5-tab SPA):**
- Measure (live reading + method-specific UI)
- Records (scrollable table with name/note edit; CSV + M5/Zeiss download)
- Jobs (switch/create/delete/download CSV)
- Report (closed-loop misclosure; CSV and HTML report export)
- Settings (method, limits, baud)

**Safety Checks:**
- Station check: |dH1 - dH2| per setup ≤ limit (configurable, default 3mm)
- Distance balance: |ΣdBS - ΣdFS| ≤ 10m (configurable)
- BFFB PASS/FAIL auto-deletes 4 records on fail, prompts retry

---

## BFFB Method — Arithmetic Reference

### Reading Sequence per Setup

```
BS1 — aim at rear staff (BM or previous TP), read staff
FS1 — turn to forward staff (new TP), read staff
FS2 — re-read forward staff (independent check)
BS2 — turn back to rear staff (instrument sinking check)
```

### Formulas

**Per-record:**

| Record | HI | RL stored |
|--------|----|-----------|
| BS1 | `prev_RL + BS1_staff` | `prev_RL` (instrument station RL) |
| FS1 | (same HI) | `HI − FS1_staff` |
| FS2 | (same HI) | `HI − FS2_staff` |
| BS2 | (same HI) | `prev_RL + BS1_staff − BS2_staff` (sinking check only) |

**Mean RL carried to next setup (critical — this is the only value that propagates):**

```
bs_mean = (BS1_staff + BS2_staff) / 2
fs_mean = (FS1_staff + FS2_staff) / 2
mean_RL = prev_RL + (bs_mean − fs_mean)
```

> **Important:** The BS2 record's stored RL is the *sinking check* only.
> The `mean_RL` is what seeds the next setup's `prev_RL`.
> `survey_recalc()` returns this value; job load uses the return value
> to seed `current_rl`, NOT the last record's stored RL.

### Example (from field test)

| Setup | prev_RL | bs_mean | fs_mean | dH | mean_RL → next |
|-------|---------|---------|---------|-----|----------------|
| Set 1 | 50.0000 | 1.0037 | 1.0389 | −0.0352 | **49.9648** |
| Set 2 | 49.9648 | 1.0876 | 1.0318 | +0.0558 | **50.0206** |
| Set 3 | 50.0206 | 1.0704 | 1.0933 | −0.0229 | **49.9977** |

---

## M5 (Zeiss/DiNi) Format Export

### Line Structure (121 bytes per line)

```
For M5|Adr    N|INFO_BLOCK            |BLOCK3                |BLOCK4                |BLOCK5                |
```

- `INFO_BLOCK` — 27 chars: `PNo(8) + Code(5) + 6sp + Sno(4) + Zno(4)` for KD1 measurement
- Each data block — 22 chars: `%-2s %14.4f %-4s` (label, value, unit `m   `)
- `M5_EMPTY` — 22 chars of spaces when block is unused

### Record Types

| Type | Description |
|------|-------------|
| `TO  Start-Line` | File header — address 1 |
| `KD1 PNo Code Sno Zno` | Measurement — Rb (BS), Rf (FS), Z (elevation) |
| `KD2 PNo Code setups Db Df Z` | Closing record — total distances, final elevation |
| `TO  End-Line` | File footer |

### BF Output Sequence (per setup)

```
KD1  ...  Z  <BM_elevation>          ← first setup only (opening BM)
KD1  ...  Rb <BS_staff>  HD <BS_dist>
KD1  ...  Rf <FS_staff>  HD <FS_dist>
KD1  ...  Z  <computed_RL>
```

### BFFB Output Sequence (per setup)

```
KD1  ...  Z  <BM_elevation>          ← first setup only
KD1  ...  Rb <BS1_staff> HD <BS1_dist>
KD1  ...  Rf <FS1_staff> HD <FS1_dist>
KD1  ...  Rf <FS2_staff> HD <FS2_dist>
KD1  ...  Rb <BS2_staff> HD <BS2_dist>
KD1  ...  Z  <mean_RL>               ← after BS2, uses mean RL not sinking-check RL
```

### KD2 Closing Record

```
KD2  LastPoint  Code  Nsetups  |Db <total_BS_dist>|Df <total_FS_dist>|Z <final_RL>|
```

### Float Precision

Values formatted as `%14.4f` (4 decimal places). Using 6dp causes visible
float arithmetic noise in the LSB (e.g., `266.390015` instead of `266.3900`).

---

## CSV Storage Format

### Header (9 columns)

```
Point,Setup,Name,Sight,Staff(m),Distance(m),HI(m),RL(m),Status
```

`sscanf` format: `"%lu,%lu,%23[^,],%3[^,],%f,%f,%f,%f,%7s"` — 9 return values.

> Note: HI and RL stored in CSV are the values at time of measurement.
> On every job load, `survey_recalc()` recomputes all HI/RL from staff
> readings. The CSV values are overwritten in memory but not rewritten
> to disk unless a void/edit operation occurs.

---

## Critical Bug Fixes (V2 session)

### RL carry-forward after BFFB setup

**Problem:** After BS2, `current_rl` was set to the sinking-check RL
(e.g. 50.0000) instead of the mean RL (e.g. 49.9648). Next setup's
BS1 computed wrong HI.

**Fix:** `job_add_point()` computes mean RL (`carry_rl`) from BS1/FS1/FS2/BS2
staff readings when BS2 is processed. `s_job.current_rl = carry_rl`.

### RL wrong when loading existing job

**Problem:** `survey_recalc()` computed correct mean RL internally but
didn't expose it. `update_current_from_records()` read the last record's
stored RL (sinking check) instead of the mean.

**Fix:** `survey_recalc()` now returns `float` — the final carry RL.
`job_init()` and `job_select()` assign this return value to `current_rl`
after calling `update_current_from_records()`.

### setup_no not restored on job load

**Problem:** `update_current_from_records()` reset `current_setup_no = 0`
and never restored it from records. Next BS1 after load incremented from
0 → 1 instead of the correct next number.

**Fix:** Restore `current_setup_no` from the last valid record in
`update_current_from_records()`.

### M5 export: BS2 distance missing from KD2 Db sum

**Problem:** `SIGHT_BS2` case in `m5_export.c` was missing `db += r->distance`.
KD2 `Db` field (total backsight distance) was under-counted by one BS2 leg.

**Fix:** Added `db += r->distance` at the top of the `SIGHT_BS2` case.

### M5 export: float precision noise at 6 decimal places

**Problem:** `%14.6f` format exposed 32-bit float arithmetic rounding
(e.g., `266.390015 m` instead of `266.3900 m`).

**Fix:** Changed format to `%14.4f` in all three data blocks in `write_line()`.
4 decimal places (0.1mm) matches DiNi instrument precision.

### M5 export: opening BM name wrong (`BM001` instead of station name)

**Problem:** `job_add_point()` auto-name generation for `SIGHT_BS` / `SIGHT_BS1`
always overwrote `r.name` and never checked `override_name`. CSV rows with
a named BM (e.g., `405`) were imported with the correct name in the CSV but
silently replaced with the auto-generated `BM001`.

**Fix:** Added `override_name` check after auto-naming block for `SIGHT_BS`,
`SIGHT_BS1`, and `SIGHT_IS`, matching the existing pattern for `SIGHT_FS`/`SIGHT_FS1`.

### ΔElev inconsistent with Comp. Elev in report

**Problem:** The server rounds `sum_bs` and `sum_fs` independently to 4 decimal
places in the JSON response. Subtracting the two rounded values in JS gave a
different result (e.g. 0.0003m) than `comp_elev − opening_bm` (0.0002m), which
is computed at full float precision before rounding.

**Fix:** `ΔElev` is now derived as `comp_elev − opening_bm` rather than
`sum_bs − sum_fs`. All three numbers (ΣBS − ΣFS = ΔElev, Opening BM + ΔElev
= Comp. Elev, Misclosure = Comp. Elev − Closing BM) are now consistent.

### HTML report row colours not distinct per instrument position

**Problem:** The HTML export coloured rows using `(setup_no − 1) % 10`. Any
records sharing the same `setup_no` got the same background — masking distinct
instrument positions when `setup_no` wasn't incrementing correctly.

**Fix:** Same sight-transition counter already used by the on-screen Records tab:
increment colour index on each `BS` / `BS1` observation, so every instrument
position gets a distinct colour regardless of stored `setup_no`.

### HTML/CSV export observation records empty

**Problem:** `cachedRecords` is populated only when the Records tab is opened.
If the user went directly to the Report tab, `calcMisclose()` set `cachedReport`
but `cachedRecords` stayed `[]`. The exported HTML/CSV had no observation rows.

**Fix:** `calcMisclose()` now `await loadRecords()` after setting `cachedReport`,
ensuring records are always cached before the user can export.

### SPIFFS silent data wipe on mount failure

**Problem:** `format_if_mount_failed = true` meant any SPIFFS mount error
(e.g. power-interrupted write, library change) silently erased all job data.

**Fix:** Changed to `false`. On mount failure the firmware logs a warning and
runs without storage instead of wiping the SPIFFS partition.

### Nav bar obscuring last record (mobile)

**Problem:** Records table had `max-height:380px` with its own scroll.
On Android, the fixed bottom nav bar overlapped the last visible row.

**Fix:**
- Removed `max-height` from records table wrapper — page scrolls as one unit
- Added `viewport-fit=cover` and `env(safe-area-inset-bottom)` to handle
  Android gesture navigation bar
- `Cache-Control: no-store` meta tag prevents browser caching stale HTML
  between firmware flashes

---

## Lessons Learned

1. **Manual is a starting point, not the truth** — always test with real hardware
2. **Android Chrome stricter than Desktop** — use desktop F12 console for debugging, always final test on phone
3. **Non-ASCII chars in JS strings cause silent SyntaxError on Android** — OK in HTML text content, NEVER inside `<script>` strings
4. **UI element removal needs null checks** — loops referencing removed DOM elements crash silently
5. **Loose solder wire looks like timeout** — always check hardware before blaming software
6. **Adaptive timeout beats fixed timeout** — SDL30 sends intermediate bytes during measurement; reset timer on byte arrival
7. **HI/RL must be recomputed on load, not just stored** — `survey_recalc()` must run on every job open; stored CSV values are stale after any bug fix
8. **The mean RL after BFFB is NOT in any record** — it lives only as the return value of `survey_recalc()`; if you lose it, the next setup starts from the sinking-check RL (wrong)
9. **`env(safe-area-inset-bottom)` is mandatory for mobile PWAs** — Android gesture bar adds invisible height below the viewport
10. **Batch fixes before flashing** — each flash requires physically swapping cable between laptop and SDL30+battery; never flash a partial fix
11. **Optimistic UI (auto-save on change) better than Save button** — field use favors fewer taps
12. **Laptop USB provides ~500mA — insufficient for ESP32 WiFi peaks** — use phone charger (2A+) for reliable power
13. **32-bit float has ~7 significant digits** — formatting RL values at 6dp reveals arithmetic noise in the LSB; always use 4dp for M5 output to match instrument precision
14. **SPIFFS survives firmware reflash** — job CSV and meta files are untouched by `idf.py flash`; the SPIFFS partition is not listed in flash_args so it is never overwritten. `idf.py flash` is safe for day-to-day development.
15. **`format_if_mount_failed = true` is a silent data destroyer** — any SPIFFS mount hiccup erases years of field data with no warning; always set `false` and handle the error explicitly
16. **JSON rounding creates arithmetic inconsistency** — when server rounds `sum_bs` and `sum_fs` independently, JS subtraction gives a different result than the server-computed `comp_elev − opening_bm`; always derive displayed deltas from the authoritative computed value
17. **`cachedRecords` must be populated before export** — if the export function relies on a cache that's only filled on tab switch, the user can export an empty report without any error; make the export trigger its own fetch

---

## V2 Status — COMPLETE

All planned V2 features implemented and field-verified.

### Verified

| Feature | Test |
|---------|------|
| BF M5 export | 19-setup BF job (`18001_section1.csv`, real DiNi reference data) |
| BFFB M5 export | 4-setup closed loop; Z after BS2 with mean RL; Db/Df correct; misclosure +0.3mm |
| BFFB arithmetic | Mean RL verified against real Trimble DiNi field file (`10032026_Rev02.DAT`) |
| Note → TO record | "Nuts on concrete foundation" emitted correctly as TO after BS1 |
| KD2 closing record | Setup count, Db, Df, final Z all correct |
| CSV report export | RW2E job — arithmetic check, distance balance, all 16 observation records |
| HTML report export | RW2E job — professional layout, per-setup colour coding, misclosure +0.2mm |
| Traffic-light LEDs | Red blink = no SDL30, Green solid = ready, Yellow blink = measuring |

### Deferred to V3

- **GPS + timestamp in M5**: Phone GPS (±10m) + measurement time in a `TO` note
  record after each Z. Genuine advantage over real DiNi (no GNSS). Timestamp
  in info block; GPS as `lat,lon,HH:MM:SS` in TO record (25 chars, fits in 27).
- **OTA firmware update**: Web UI upload endpoint to eliminate cable-swap.
- **Battery pack**: Hardware-only concern — use 2A+ charger; PL4506+MT3608 combo board's built-in power button is the on/off switch. No firmware changes needed.

### Known non-blocking issues

- **`setup_no` in pre-fix CSVs**: Jobs recorded before the setup_no fix show
  wrong setup numbers in KD2 count. Staff readings are ground truth; RL is
  unaffected. New jobs are correct.
- **`Sh` summary record**: Real DiNi emits a total ΔH + misclosure record
  before KD2. Our exporter omits it. TBC imports correctly without it.

---

## Test Tools

### `tools/import_csv.py` — CSV import (test only)

Posts a job CSV to the `/api/import` endpoint. Used to inject reference data
(e.g., TBC-exported CSV) for M5 export verification.

```
python3 tools/import_csv.py <csv_file> <job_name> [device_ip]
```

The `/api/import` POST endpoint (`web_server.c`) is test-only and not
linked from the web UI. It reads bench RL from CSV column 7, creates the
job, then adds each row via `job_add_point()`.

---

## Git History

```
83bd9ec Feat: traffic-light status LEDs (Red=GPIO4, Yellow=GPIO5, Green=GPIO6)
fd37057 Docs: update V2 log — report export verified, 4 new bug fixes documented
d3ed13c Fix: HTML/CSV export empty records + SPIFFS silent wipe prevention
18e8cd8 Fix: HTML report row colours by sight transition, not setup_no
69d6fbe Fix: ΔElev derived from comp_elev−opening_bm to match Misclosure
6899fbd Fix: carry-RL bug in delete/edit/bench ops + Report CSV/HTML export
2224a69 Docs: mark V2 complete — BFFB M5 export verified
ba37fdf Feat: M5 export tested+fixed, UX cleanup, /api/import test endpoint
f43eb0f Fix: Records page BS-FS display and station PASS/FAIL for BFFB
3698ef7 Fix: paired name sync (BS1↔BS2, FS1↔FS2) and Report misclosure/points
215f7e8 Feat: editable FS point name field + sequential TP naming
78b85e4 Docs: add V2 development log for ESP32-S3-N16
4b45b11 Fix: RL/HI carry-forward, setup_no restore, nav bar, TP naming
2ea206b Fix: clear measurement display on new job
971f11b Fix: dblr_step reset to BS1 on new job
c91426e Fix: URI handlers 23/25, WS2812 init attempt (LED deferred)
3719439 Fix: setup_no assignment + sight reset on new job
5122212 V2: Port to ESP32-S3-N16
```

---

## Field Test Protocol

1. Short closed loop (6-8 setups) returning to opening BM
2. Target misclosure: ≤ 2mm per setup (3rd order)
3. Compare with Sokkia SDR33 on same line if available
4. Test in hot weather (60m distance) — verify adaptive timeout
5. Test battery life — target 8+ hours continuous use
6. Verify RL carry-forward across setups (check BS1 RL of each new setup)
