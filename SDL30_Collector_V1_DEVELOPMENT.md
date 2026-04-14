# SDL30 Collector V1 — Development Journal

> **"New life for an old instrument"**  
> A case study: replacing the broken Topcon SDR33 data collector  
> with an ESP32-based web UI for under ฿1,000.

---

## Background

The **Topcon SDL30** is a high-quality digital level (barcode staff reader) manufactured around 1995. Its original companion data collector was the **Topcon SDR33** — a dedicated handheld device costing over $1,000 USD to replace. When the SDR33 breaks or is lost, the SDL30 becomes unusable for data recording despite being a perfectly functional precision instrument.

This project replaces the SDR33 with:
- **ESP32-WROOM-32** (~฿200)
- **SP3232EEN RS232-TTL breakboard** (~฿50)
- **USB-to-Hirose-6-pin cable** (custom)
- **Web UI** served from ESP32 WiFi AP

Total cost: **under ฿1,000** vs $1,000+ for SDR33 replacement.

---

## Hardware

### Components

| Component | Description |
|-----------|-------------|
| ESP32-WROOM-32 | Main controller, WiFi AP |
| SP3232EEN breakboard | RS232 ↔ TTL level converter |
| Hirose HR10A-7P-6P | 6-pin circular connector on SDL30 |
| USB-A to Hirose cable | Custom cable, Prolific PL2303 chip |

### SDL30 RS-232 Port

The SDL30 has a **Hirose HR10A-7P-6P** 6-pin circular connector on the back panel.

> ⚠️ **CRITICAL WARNING — Pin numbering:**  
> Pin 1 is **counter-clockwise** from the key tab when viewing the connector face-on.  
> Many datasheets show clockwise — this is WRONG for this connector.  
> Confirmed correct by field testing.

**Pin mapping (face view, counter-clockwise from key):**

```
Pin 1 = Black  → GND
Pin 2 = NC
Pin 3 = White  → SDL30 TXD (data OUT from SDL30)
Pin 4 = Green  → SDL30 RXD (data IN to SDL30)
Pin 5 = Red    → NC
Pin 6 = NC
```

### SP3232EEN Wiring

```
SP3232EEN RS232 side ← SDL30 cable:
  RS232 RXD ← Pin 3 White (SDL30 TXD)
  RS232 TXD → Pin 4 Green (SDL30 RXD)
  GND       ← Pin 1 Black

SP3232EEN TTL side → ESP32:
  TTL TXD (White) → GPIO16 (ESP32 UART2 RX)
  TTL RXD (Green) → GPIO17 (ESP32 UART2 TX)
  GND    (Red)    → GND
  VCC    (Blue)   → 3.3V
```

---

## SDL30 Protocol Findings

### Communication Settings
```
Baud rate : 2400 (NOT 1200 — confirmed by testing)
Data bits : 8
Parity    : None
Stop bits : 1
Flow ctrl : None
```

> ⚠️ SDL30 config: MENU → 4.Config → 4.RS-232 → set Baud=2400, Parity=N

### Commands

| Command | Response | Notes |
|---------|----------|-------|
| `LA\r` | `LA SDL30,001786,1112\r\n` | Model, serial, ROM version |
| `LM\r` | `LM 0.7890,1.88\r\n` | Staff (Rh), Distance (Hd) |
| `LT\r` | *(no response)* | Stop — silent in Single mode |
| `LB\r` | `LB 0,0\r\n` | Parameters |

### Real Response Format (CRITICAL)

```
WRONG (documented format):  "LM +1.2345, 023.456"
CORRECT (actual SDL30 V1):  "LM 0.7890,1.88"
  - Space after "LM" prefix
  - No "+" sign on positive values
  - No leading zeros on distance
  - Comma without space separator
```

### Operating Screen

The SDL30 must be on the **Ht-diff standby screen** to respond to `LM\r`:
```
Meas
     Rh    xxxx m
S  Hd    yyyy m
```

The sub-menu screen (Ht-diff / Set-out / Elev. / Config) appears only when **MENU button** is accidentally pressed. Press **ESC** to return to standby. The `LM\r` command works from both screens — SDL30 returns to standby screen after measurement.

### LA Startup Check

At boot, ESP32 sends `LA\r` to confirm SDL30 is connected:
```
Success → "SDL30 connected: SDL30 SN:001786 ROM:1112"
Failure → "SDL30 not found — check: powered on? standby screen? baud=2400?"
```

After 3 consecutive LM timeouts → ESP32 re-sends `LA\r` to confirm connection.

---

## Software Architecture

### Framework
- **ESP-IDF v5.4** (not Arduino)
- **FreeRTOS** tasks
- **SPIFFS** filesystem (875KB available)
- **ESP HTTP Server** for web UI
- **cJSON** for API responses

### Project Structure

```
sdl30_collector/
├── CMakeLists.txt
├── sdkconfig.defaults
├── partitions.csv
└── main/
    ├── CMakeLists.txt
    ├── config.h          ← baud, GPIO, WiFi, defaults
    ├── main.c            ← app_main, SDL30 monitor task
    ├── sdl30_types.h     ← sight_type_t enum
    ├── settings.h/c      ← survey settings, SPIFFS persistence
    ├── sdl30/
    │   ├── sdl30.h/c     ← UART protocol, LM/LA/LB commands
    ├── survey/
    │   ├── survey.h/c    ← leveling arithmetic, misclose
    │   ├── job.h/c       ← job management, records, SPIFFS CSV
    ├── storage/
    │   ├── storage.h/c   ← SPIFFS init, file operations
    └── web/
        ├── web_server.h/c ← WiFi AP, HTTP REST API
        └── web_ui.h       ← full 5-tab SPA (embedded HTML/CSS/JS)
```

### WiFi Configuration
```
SSID     : SDL30_Collector
Password : survey1234
IP       : 192.168.4.1
Open browser → http://192.168.4.1
```

---

## Web UI — 5 Tabs

### Tab 1: Measure
- Current status: Job, HI, RL, Readings, Points
- Sight type: **[BS]** **[FS]** (leveling) or **[BS]** **[FS]** **[IS peg check]** (SET-OUT)
- Auto-advance: BS→FS, FS→BS
- Large staff reading display with RL and HI
- Reconnects WiFi on screen wake (visibilitychange event)

### Tab 2: Records
Two sections (vertical scroll):

**Section 1 — Running summary (side by side):**
```
Arithmetic          Dist Balance
Σ BS  x.xxxx m     Σ BS dist  xx.xxx m
Σ FS  x.xxxx m     Σ FS dist  xx.xxx m
ΣBS−ΣFS x.xxxx m   Diff       xx.xxx m
BS-FS   x.xxxx m   Limit      xx.xxx m
1st Elev x.xxxx m  PASS/FAIL
Last RL/HI x.xxxx m
(blank for BF — no PASS/FAIL)
```

**Section 2 — Observation table:**
- Columns: #, Sight, Staff(m), Dist(m), HI(m), RL(m), [delete]
- Colour coding: BS=blue, FS=green, IS=white
- Delete button with immediate recalculation

### Tab 3: Jobs
- Active job info (name, BM, points, download CSV)
- Set benchmark RL
- Create new job
- Storage bar (SPIFFS usage)
- All jobs list with select/delete

### Tab 4: Report
**Misclosure Report** (after entering closing BM):
```
Opening BM + ΣBS − ΣFS = Comp. Elev
Σ BS          +x.xxxx m
Σ FS          +x.xxxx m
ΔElev         ±x.xxxx m
Comp. Elev     x.xxxx m
Closing BM     x.xxxx m
Misclosure    ±x.xxxx m (x.x mm)  [green/orange/red]
Points         n
Readings       n
```

**Distance Report:**
```
ΣBS Dist + ΣFS Dist = Total Dist
Σ BS dist   xx.xxx m
Σ FS dist   xx.xxx m
Dist Balance xx.xxx m < OK / > FAIL
Total dist   xx.xxx m
```

**Export:** CSV ✅ | GSI-16 (coming) | M5/Zeiss (coming)

### Tab 5: Settings
- Observation method: BF / BFFB / BFBF / BBFF / SET-OUT
- Limits: max station diff (mm), max dist diff (m), max cum dist diff (m), min sight ht (m), max sight dist (m)
- Alerts: alert on exceed, block save on exceed
- Baud rate: 2400 / 1200

---

## Observation Methods

| Method | Order | Description | PASS/FAIL |
|--------|-------|-------------|-----------|
| BF | Basic | BS, FS, BS, FS... | Arithmetic only |
| BFFB | 3rd | BS, FS, FS, BS per setup | Phase 2 |
| BFBF | 2nd | BS, FS, BS, FS per setup | Phase 2 |
| BBFF | 1st | BS, BS, FS, FS per setup | Phase 2 |
| SET-OUT | Construction | BS + IS peg checks | Design RL |

> **Note:** BFFB/BFBF/BBFF double-reading methods are Phase 2. BF is fully implemented.

---

## REST API Endpoints

| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/` | Web UI HTML |
| GET | `/api/status` | Job, HI, RL, readings, points, SDL30 status |
| GET | `/api/records` | All observation records |
| GET | `/api/jobs` | Job list + storage info |
| GET | `/api/download` | CSV file download |
| GET | `/api/misclose?closing_rl=x` | Misclosure calculation |
| GET | `/api/settings` | Current settings |
| POST | `/api/measure` | Trigger SDL30 measurement |
| POST | `/api/sight` | Set next sight type |
| POST | `/api/job/new` | Create new job |
| POST | `/api/job/select` | Select active job |
| POST | `/api/job/bench` | Set benchmark RL |
| POST | `/api/job/delete` | Delete job |
| POST | `/api/record/delete` | Delete a record |
| POST | `/api/settings` | Save settings |

---

## CSV Export Format

```csv
Point,Sight,Staff(m),Distance(m),HI(m),RL(m),Status
1,BS,0.8596,1.870,100.8596,100.0000,OK
2,FS,0.6871,1.860,100.8596,100.1725,OK
3,BS,0.9506,1.780,101.1231,100.1725,OK
4,FS,1.1304,6.920,101.1231,99.9927,OK
```

---

## Build & Flash

```bash
# Load ESP-IDF
get_idf   # alias in ~/.zshrc

# Build
cd ~/dev/sdl30_collector
idf.py build

# Flash and monitor
idf.py -p /dev/ttyUSB0 flash monitor

# Build errors only
idf.py build 2>&1 | grep "error:" | head -20
```

---

## Known Issues & Solutions

### WiFi Reconnection After Phone Sleep
**Problem:** Android suspends browser after ~90 seconds of phone idle, causing LM timeout on next MEASURE tap.

**Solution implemented:** `visibilitychange` and `focus` event listeners pre-warm WiFi when phone screen turns on — before user taps MEASURE.

**Workaround if still occurs:** Refresh browser page once, then MEASURE works immediately.

### SDL30 LM Timeout
**Problem:** Occasional LM timeout despite SDL30 being on correct screen.

**Root cause:** Android WiFi power saving — WiFi reconnection takes 1-2 LM timeout cycles (~10-20 seconds).

**Self-healing:** After 1-2 timeout retries, WiFi reconnects and measurement succeeds automatically.

### xtensa uint32_t Format Specifier
All `uint32_t` values in ESP_LOG must use `%lu` with `(unsigned long)` cast, not `%u`.

---

## Field Test Results

### Test 1 — Closed loop (4 points, 6 readings)
```
Opening BM   : 100.0000m
Closing BM   : 100.0000m
Comp. Elev   : 99.9373m
Misclosure   : -0.0007m (0.7mm) ← GREEN ✅
Total dist   : 7.700m
Dist Balance : 0.000m ← PASS ✅
```

### Test 2 — House floor survey (4 points, 6 readings)
```
Opening BM   : 10.0000m
Closing BM   : 10.0000m
Comp. Elev   : 10.0003m
Misclosure   : +0.0003m (0.3mm) ← GREEN ✅ 🏆
Total dist   : 21.000m
Dist Balance : 0.500m ← PASS ✅
Floor level diff: 7.3mm corner to corner
```

**0.3mm misclosure confirms SDL30 precision is excellent** even as second-hand instrument.

---

## Pending / Future Work

### Phase 1 (current)
- [ ] GSI-16 export
- [ ] M5/Zeiss export  
- [ ] CSV 4 decimal places for round numbers
- [ ] SET-OUT mode full implementation

### Phase 2
- [ ] BFFB double-reading observation method
- [ ] BFBF double-reading observation method
- [ ] BBFF first-order observation method
- [ ] Station diff PASS/FAIL for double-reading methods

### Phase 3
- [ ] Point name/description field
- [ ] Leica GSI-8 export
- [ ] Bowditch adjustment
- [ ] Multiple benchmark support

---

## Version History

| Version | Date | Notes |
|---------|------|-------|
| V1.0 | April 2026 | Initial release — BF method, 5-tab web UI, CSV export |

---

## Credits

- **Instrument:** Topcon SDL30 Digital Level (SN: 001786, ROM: 1112)
- **Developer:** Prajuab (pbroboto) — 30 years surveying experience
- **Platform:** ESP32-WROOM-32 + ESP-IDF v5.4
- **Inspiration:** Broken Topcon SDR33 data collector
- **Location:** Thailand (U-Tapao International Airport project)

---

*"New life for an old instrument — the SDL30 lives again!"* 🎉
