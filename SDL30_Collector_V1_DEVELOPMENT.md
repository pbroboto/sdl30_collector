# SDL30 Collector — Development Log

## Overview

ESP32-based wireless data collector for Sokkia SDL30 digital level, replacing the obsolete SDR33 data collector (original cost ~$1,000+ USD) with a DIY solution under ฿1,000 THB.

**Target device:** Sokkia SDL30 SN:001786, ROM 1112 (older model without name field support)

**Mission:** Add modern features (point names, digital workflows, BFFB method) that newer SDL30 firmware versions have, but this older unit lacks.

---

## Hardware

### V1 — ESP32-WROOM-32

| Component | Part | Price (THB) |
|-----------|------|-------------|
| MCU | ESP32-WROOM-32 DevKit | 80-120 |
| RS-232 level shifter | SP3232EEN module | 40 |
| Cable | USB-A to Hirose 6-pin | 200 |
| Battery | 18650 3200mAh | 150 |
| Battery board | PL4506+MT3608 all-in-one | 80 |
| Enclosure, wiring | ABS box 120×97×40mm | 100 |
| **TOTAL** | | **~650-700 THB** |

### V2 — ESP32-S3-N16 (current)

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
- Records (scrollable table with name/note edit)
- Jobs (switch/create/delete/download CSV)
- Report (closed-loop misclosure, M5 export)
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

---

## Pending / Known Issues

- **TP naming**: leapfrog (TP001/TP002 alternating) implemented but needs
  rethink. General case requires sequential numbering (TP001, TP002, TP003…)
  with ability to set a custom name (e.g. ST35) when FS is a named benchmark.
  Proposed: editable name field in Measure tab pre-populated with next TP.
- **setup_no in existing CSV**: records saved before the setup_no fix show
  wrong numbers. `survey_recalc()` does not correct setup_no — only staff
  readings are ground truth.
- **OTA firmware update**: partition table has OTA slots. Adding a web UI
  upload endpoint would eliminate cable-swap for field updates.

---

## Git History

```
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
