# ereader_rev1 — Parts Checklist / BOM Tracker

Living checklist of the board's non-trivial parts (skip generic caps/resistors/ferrites). Parts live in shared functional libraries under `libs/symbols/`, `libs/footprints/`, and `libs/3dmodels/`; see `LIBRARY_STANDARDS.md`. Library organization updated 2026-09-05; part-selection entries below retain their previous status.

Priority key: 🔴 essential · 🟡 blocked/pending a decision · 🟢 standard · ⚪ optional.

---

## ✅ In the library (sourced + symbol/footprint/3D added)
- [x] **Host MCU** — `R7KA8P1KFLCAC#UC0` (Renesas RA8P1, 289-pin BGA; M85+M33+Ethos-U55 NPU)
- [x] **Radio** — `ESP32-C6-WROOM-1-N8` (Wi-Fi6/BLE5.3/Thread; link to RA8 via SDIO/ESP-Hosted)
- [x] **SDRAM (framebuffer)** — `IME5132SDBETG-6I` (Intelligent Memory, 512 Mb 16M×32 SDR, on-die ECC, TSOP-II-86)
- [x] **Accelerometer / auto-rotate** — `ADXL367BCCZ-RL7` (Analog Devices, 3-axis **nanopower**, 14-bit, hardware single/double-tap, **SPI or I²C**) — successor to the ADXL362
  _(the 2 legacy RA8D2 symbols were dropped in the monorepo refactor)_

---

## 🔴 Power subsystem — *nothing sourced yet; all essential*
- [x] **Li-ion charger (power-path)** — `BQ25188YBGR` (TI, 1 A single-cell, I²C, WLCSP-8) ✅ in library
- [x] **Fuel gauge** (2 in library — pick one) — `MAX17260SETD+T` (ADI ModelGauge m5, ⚠️ needs external **~10 mΩ shunt**) **or** `BQ27427YZFR` (TI, **integrated** sense resistor — no shunt)
- [x] **3.3 V rail — BUCK-BOOST** — `TPS63802DLAT` (TI, 2 A, **adjustable → set to 3.3 V** via FB divider) ✅ in library; buck-boost required (Li-ion 3.0–4.2 V straddles 3.3 V, 4.2 V > RA8 max)
- [x] **1.8 V rail** — `TPS7A0218PYCHR` (TI fixed **1.8 V** LDO, 200 mA, 25 nA Iq, 4-DSBGA, fed from 3.3 V) ✅ in library
- [ ] **Battery connector** (+ protection IC if the cell has none)
- [x] **USB-C receptacle + ESD array** — `12401610E4#2A` (Amphenol USB-C 3.2 Gen2) + `RCLAMP0582N.TCT` (Semtech low-cap ESD) ✅ in library; ⚪ optional USB-PD controller
- [x] **Load switch** (power-gate the ESP32-C6) — `TPS22918TDBVRQ1` (TI, 2 A, SOT-23-6) ✅ in library

## 🟡 Display subsystem — *blocked on picking the e-ink panel (heart of the reader)*
- [ ] **E-ink panel** — NOT chosen yet (own future decision; see project notes)
- [ ] **IT8951 e-paper controller** (external-controller route) — or comes on a Waveshare board
- [ ] **EPD power PMIC** (±15 V / VCOM) — TI TPS65185 / Silergy SY7636A (skip if IT8951 board supplies it)
- [ ] **FPC connector(s)** for panel (+ touch)
- [ ] **Capacitive touch controller** (I²C) — GT911 / FT5x06 (if touchscreen)

## 🔴 Storage & code memory
- [x] **QSPI NOR flash** (firmware + fonts + OCR model) — `IS25LP01GJ-RHLE` (ISSI, **1 Gbit / 128 MB**, Quad-DTR 166 MHz, 3.3 V, 36 mA, 24-TFBGA) ✅ in library
- [x] **User book storage** — `DM3AT-SF-PEJM5` (Hirose microSD socket, push-push + card-detect, SMT) ✅ in library
- [x] **Internal data storage** — `MX35LF1G24AD-Z4I-T` (Macronix SPI-NAND, **128 MB**, Quad, on-die ECC, **3.3 V**, WSON-8) → default books + persistent cache ✅ in library
- [x] **SD ESD array** — `TPD6E004RSER` (TI, 6-channel, 0.5 pF low-cap, ±15 kV, UQFN-8) on the microSD lines ✅ in library

## 🟡 Front light — *defining reader feature*
- [ ] **LED boost driver** — TI LM3630A (or similar) + edge-lit front-light LEDs (white; ⚪ amber for warm mode)

## 🔴 Clocks
- [x] **Main crystal** — `FL2400022` (24 MHz, 10 pF, 10 ppm, SMD 4-pad) ✅ in library
- [x] **32.768 kHz RTC crystal** — `ABS07-32.768KHZ-1-T` (Abracon, 12.5 pF, 10 ppm, 2-pin) ✅ in library

## 🟢 Input & debug
- [ ] **Power/reset button** + page-turn tactile buttons
- [ ] **SWD debug header** (Tag-Connect or 2×5)

## ⚪ Optional / nice-to-have
- [ ] **Ambient light sensor** (auto front-light brightness) — VEML7700
- [ ] **Hall sensor** (magnetic case-cover → auto sleep/wake)
- [ ] **Audio** (TTS / audiobooks) — codec + amp + speaker, or do it over BLE

---

## Notes
- **Power + clock reference:** consult `resources/manuals/Renesas_EK-RA8D2_Board_BOM.xlsx`, but verify regulators, crystals, decoupling and pin compatibility against the exact RA8P1 ordering code. The RA8D2 reference does not guarantee compatibility.
- **Two urgent gaps:** the entire **power chain** (nothing yet) and the **display subsystem** (unblocks once the e-ink panel is chosen).
- Every new part: source it, import its symbol/footprint/model into the functional directories under `libs/` per `LIBRARY_STANDARDS.md`, then check it off here.
