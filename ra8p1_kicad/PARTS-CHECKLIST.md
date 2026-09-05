# ereader_rev1 — Parts Checklist / BOM Tracker

Living candidate inventory, not an approved purchasing list. Library presence does not establish electrical suitability, availability, or footprint qualification. Parts live in shared functional libraries under `libs/symbols/`, `libs/footprints/`, and `libs/3dmodels/`; see `LIBRARY_STANDARDS.md`.

## Schematic-linked BOM and sourcing policy

The current placed-parts BOM is [exports/ereader_rev1_bom.csv](exports/ereader_rev1_bom.csv), exported through KiCad's Symbol Fields Table for the entire project. It includes all physical components, including capacitors, resistors, and switches. Unplaced library candidates below are not included in its quantities. The incomplete schematic is not ready to order.

Maintain manufacturer, exact ordering MPN, DigiKey part number/link, procurement status, and a dated stock/price snapshot in the schematic fields as each section is designed. Export the CSV after changes using Tools > Generate Bill of Materials, entire project, with the procurement fields included. Output is `../exports/ereader_rev1_bom.csv` relative to the schematic directory. Do not hand-edit quantities in the generated CSV.

Prefer active parts with substantial distributor-held stock, reasonable prototype and volume pricing, and adequate electrical margins. Record packaging and price breaks; do not confuse reel pricing with cut tape or marketplace stock with DigiKey inventory. Indexed stock is a dated indication, not a reservation or checkout verification. Recheck before ordering. Qualify voltage/current/temperature ratings, capacitor DC bias, regulator stability, oscillator matching, and interface compatibility before approving a selection. PCB footprint validation remains deferred.

Initial sourcing review, 2026-09-05:

- **U1 HOLD:** [DigiKey's current #UC0 listing](https://www.digikey.com/en/products/detail/renesas-electronics-corporation/R7KA8P1KFLCAC-UC0/26738126) reports discontinued at DigiKey, one in stock, USD 29.47, and no backorders. This does not establish Renesas end-of-life status. Qualify an available RA8P1 ordering suffix before release; do not silently substitute. The [#BC1 listing](https://www.digikey.com/en/products/detail/renesas-electronics-corporation/R7KA8P1KFLCAC-BC1/29277563) is active but not stocked, with an 18-week indicated lead time; equivalence is not yet established.
- **L1 candidate:** [TDK SPM5020T-2R2M-LR](https://www.digikey.com/en/products/detail/tdk/SPM5020T-2R2M-LR/5962361), DigiKey `445-174497-1-ND`, indexed stock 1,149; cut-tape unit prices USD 1.35 / 1.111 / 0.9134 at quantities 1 / 10 / 100. Active, indicated 15-week lead time. Renesas reference recommendation supports selection, but system validation is still pending.
- Remaining placed parts are explicitly marked TBD or HOLD until exact MPN sourcing and electrical qualification are completed. No fabricated total cost is reported while selections are incomplete.

Priority key: 🔴 essential · 🟡 blocked/pending a decision · 🟢 standard · ⚪ optional.

---

## ✅ In the library (imported candidates, not purchasing approval)
- [x] **Host MCU** — `R7KA8P1KFLCAC#UC0` (Renesas RA8P1, 289-pin BGA; M85+M33+Ethos-U55 NPU)
- [x] **Radio** — `ESP32-C6-WROOM-1-N8` (Wi-Fi6/BLE5.3/Thread; link to RA8 via SDIO/ESP-Hosted)
- [x] **SDRAM (framebuffer)** — `IME5132SDBETG-6I` (Intelligent Memory, 512 Mb 16M×32 SDR, on-die ECC, TSOP-II-86)
- [x] **Accelerometer / auto-rotate** — `ADXL367BCCZ-RL7` (Analog Devices, 3-axis **nanopower**, 14-bit, hardware single/double-tap, **SPI or I²C**) — successor to the ADXL362
  _(the 2 legacy RA8D2 symbols were dropped in the monorepo refactor)_

---

## 🔴 Power subsystem — *imported candidates; qualification pending*
- [x] **Li-ion charger (power-path)** — `BQ25188YBGR` (TI, 1 A single-cell, I²C, WLCSP-8) ✅ in library
- [x] **Fuel gauge** (2 in library — pick one) — `MAX17260SETD+T` (ADI ModelGauge m5, ⚠️ needs external **~10 mΩ shunt**) **or** `BQ27427YZFR` (TI, **integrated** sense resistor — no shunt)
- [x] **3.3 V rail — BUCK-BOOST** — `TPS63802DLAT` (TI, 2 A, **adjustable → set to 3.3 V** via FB divider) ✅ in library; buck-boost required (Li-ion 3.0–4.2 V straddles 3.3 V, 4.2 V > RA8 max)
- [x] **1.8 V rail** — `TPS7A0218PYCHR` (TI fixed **1.8 V** LDO, 200 mA, 25 nA Iq, 4-DSBGA, fed from 3.3 V) ✅ in library
- [ ] **Battery connector** (+ protection IC if the cell has none)
- [x] **USB-C receptacle + ESD array** — `12401610E4#2A` (Amphenol USB-C 3.2 Gen2) + `RCLAMP0582N.TCT` (Semtech low-cap ESD) ✅ in library; ⚪ optional USB-PD controller
- [x] **Load switch** (power-gate the ESP32-C6) — `TPS22918TDBVRQ1` (TI, 2 A, SOT-23-6) ✅ in library

## 🟡 Display subsystem — *blocked on picking the e-ink panel (heart of the reader)*
- [ ] **E-ink panel** — NOT chosen yet (own future decision; see project notes)
- [ ] **IT8951 e-paper controller** - integrate on this PCB; exact variant and supporting circuitry pending. Waveshare driver board is prototype-only.
- [ ] **EPD power PMIC** - integrate panel HV and VCOM generation on this PCB; qualify candidate against the selected panel, sequencing and current requirements.
- [ ] **FPC connector(s)** for panel (+ touch)
- [ ] **Capacitive touch controller** (I²C) — GT911 / FT5x06 (if touchscreen)

## 🔴 Storage & code memory
- [x] **QSPI NOR flash** (firmware + fonts + OCR model) — `IS25LP01GJ-RHLE` (ISSI, **1 Gbit / 128 MB**, Quad-DTR 166 MHz, 3.3 V, 36 mA, 24-TFBGA) ✅ in library
- [x] **User book storage** — `DM3AT-SF-PEJM5` (Hirose microSD socket, push-push + card-detect, SMT) ✅ in library
- [x] **Internal data storage** — `MX35LF1G24AD-Z4I-T` (Macronix SPI-NAND, **128 MB**, Quad, on-die ECC, **3.3 V**, WSON-8) → default books + persistent cache ✅ in library
- [x] **SD ESD array** — `TPD6E004RSER` (TI, 6-channel, 0.5 pF low-cap, ±15 kV, UQFN-8) on the microSD lines ✅ in library

## 🟡 Front light — *defining reader feature*
- [ ] **LED driver circuitry** - independently adjustable warm and cool front-light channels are required; qualify driver and LED topology against the selected panel assembly.

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
- **Urgent gaps:** MCU ordering-code availability, power-chain qualification, and the display subsystem's exact panel/controller requirements.
- Every new physical part: qualify its electrical role and DigiKey sourcing, maintain the schematic procurement fields and exported BOM, and import necessary library assets per `LIBRARY_STANDARDS.md`. Checking off a library import is not purchasing approval.
