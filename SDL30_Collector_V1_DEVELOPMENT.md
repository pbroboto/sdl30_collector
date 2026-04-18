# SDL30 Collector V1 — Development Log

## Overview

ESP32-based wireless data collector for Sokkia SDL30 digital level, replacing the obsolete SDR33 data collector (original cost ~$1,000+ USD) with a DIY solution under ฿1,000 THB.

**Target device:** Sokkia SDL30 SN:001786, ROM 1112 (older model without name field support)

**Mission:** Add modern features (point names, digital workflows, BFFB method) that newer SDL30 firmware versions have, but this older unit lacks.

---

## Hardware

### Bill of Materials

| Component | Part | Price (THB) |
|-----------|------|-------------|
| MCU | ESP32-WROOM-32 DevKit | 80-120 |
| RS-232 level shifter | SP3232EEN module | 40 |
| Cable | USB-A to Hirose 6-pin | 200 |
| Battery | 18650 3200mAh | 150 |
| Battery board | PL4506+MT3608 all-in-one | 80 |
| Enclosure, wiring | ABS box 120×97×40mm | 100 |
| **TOTAL** | | **~650-700 THB** |

### Wiring

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
│   ├── survey.c/h          # BFFB state machine, PASS/FAIL logic
├── storage/
│   ├── storage.c/h         # SPIFFS, CSV append/rewrite, meta files
└── web/
    ├── web_server.c/h      # REST API endpoints
    └── web_ui.h            # Single-page web app (HTML/CSS/JS in C string)
```

### Features

**Observation Methods:**
- **BF** (Basic/Fast) — single BS + FS per setup
- **BFFB** (3rd order) — BS1, FS1, FS2, BS2 with sinking check

**Point Name System:**
- Auto-generated: BM001 (first BS), TP001/TP002... (each new FS)
- BS2 copies from BS1 of current setup (same BM)
- FS2 copies from FS1 of current setup (same TP)
- Manual edit via web UI (tap name cell → prompt)
- `name[24]` field in `record_t` struct

**Job Management:**
- Multiple jobs with meta file (bench_rl, point_count)
- CSV auto-saves each record
- Restore job on reboot
- Jobs tab for switch/delete

**Web UI (5-tab SPA):**
- Measure (live reading + method-specific UI)
- Records (scrollable table with edit)
- Jobs (switch/create/delete)
- Report (closed-loop misclosure)
- Settings (method, limits, baud)

**Safety Checks:**
- Station check: |BS-FS| per setup ≤ 2mm (configurable)
- Distance balance: |ΣdBS-ΣdFS| ≤ 10m (configurable)
- BFFB double-reading check: |BS1-BS2| and |FS1-FS2| ≤ limit
- BFFB sinking check: BS2 RL vs opening BM

---

## BFFB Method Details

### Reading Sequence

```
Setup N:
  BS1 — aim at BM, read staff
  FS1 — turn to TP, read staff
  FS2 — re-read TP (check)
  BS2 — turn back to BM (sinking check)
```

### Arithmetic

```
Per-setup mean staff:
  BS_mean = (BS1 + BS2) / 2
  FS_mean = (FS1 + FS2) / 2

Elevation change per setup:
  ΔH = BS_mean - FS_mean

HI after setup:
  HI = previous_RL + BS_mean

TP RL:
  TP_RL = HI - FS_mean

BS2 sinking check RL:
  BS2_RL = HI - BS2_staff
  Compare to BM_RL — if differs > 1-2mm, tripod sank
```

### PASS/FAIL Criteria

```
Station check: |BS1 - FS1| reading spread within limit
Double-reading: |BS1-BS2| and |FS1-FS2| ≤ 0.001m typical
Sinking check: |BS2_RL - BM_RL| ≤ max_station_mm
```

---

## Critical Build Fixes

### Buffer sizes (Armbian/ESP-IDF)

```bash
python3 -c "
c = open('main/web/web_server.c').read()
c = c.replace('char path[80]', 'char path[320]')
c = c.replace('char path[128]', 'char path[320]')
c = c.replace('char row[128]', 'char row[512]')
open('main/web/web_server.c','w').write(c)
"
```

### Format specifiers (xtensa toolchain)

`uint32_t` must use `%lu` with `(unsigned long)` cast, not `%u`.

### SPIFFS CSV format (8 columns)

```
Point, Name, Sight, Staff(m), Distance(m), HI(m), RL(m), Status
```

`sscanf` format: `"%lu,%23[^,],%3[^,],%f,%f,%f,%f,%7s"` expecting 8 return values.

---

## Lessons Learned

1. **Manual is a starting point, not the truth** — always test with real hardware
2. **Android Chrome stricter than Desktop** — use desktop F12 console for debugging, always final test on phone
3. **Non-ASCII chars in JS strings cause silent SyntaxError on Android** — OK in HTML text content, NEVER inside `<script>` strings
4. **UI element removal needs null checks** — loops referencing removed DOM elements crash silently
5. **Loose solder wire looks like timeout** — always check hardware before blaming software
6. **Adaptive timeout beats fixed timeout** — SDL30 sends intermediate bytes during measurement; reset timer on byte arrival
7. **HI/RL should be computed on display, not stored** — allows fixes to apply to existing data
8. **Optimistic UI (auto-save on change) better than Save button** — field use favors fewer taps
9. **Laptop USB provides ~500mA — insufficient for ESP32 WiFi peaks** — use phone charger (2A+) for reliable power

---

## Git History

```
25bdb69 Add Point Name feature (name[24] in record_t)
d17fa1b Fix setMethodUI crash when mb-2/mb-3 buttons removed
d73be01 BFFB UI improvements - remove BFBF/BBFF, fix arithmetic, scroll records
a07c178 BFFB UI - DiNi style highlight, combined sight card, auto-save settings
7026863 Add BFFB/BFBF/BBFF double-reading methods
5f95db5 Restore working web_ui.h and web_server.c before BFFB UI
604f1e4 Fix Jobs tab - custom modal, data-name onclick, meta scan
771e2f9 Add setup colour coding, delete warning, job restore on reboot
c68bc28 SDL30 Collector V1 - initial release
```

---

## V2 Roadmap (ESP32-S3-N16)

- 16MB flash → 13MB SPIFFS → 295,000 records possible
- 8MB PSRAM → larger web UI without size concerns
- Dual-core LX7 @ 240MHz → faster UI
- 3-LED status (🔴 error, 🟡 WiFi, 🟢 SDL30)
- Active buzzer (PASS/FAIL/measure beeps)
- USB-OTG support (direct PC connection possible)
- Same GPIO 16/17 UART pins for code compatibility

---

## Field Test Protocol

1. Short closed loop (6-8 setups) returning to opening BM
2. Target misclosure: ≤ 2mm per setup (3rd order)
3. Compare with Sokkia SDR33 on same line if available
4. Test in hot weather (60m distance) — verify adaptive timeout
5. Test battery life — target 8+ hours continuous use

