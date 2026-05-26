# EK-RA8D2 OEM_PL0 Brick: Recovery Procedure

**Date of incident:** 2026-05-25
**Chip state at end of session:** DLM = `OEM_PL0`, debug access entirely denied
**Cause:** Erroneous `rfp-cli -dlm OEM_PL0` transition from `OEM_PL2` while
attempting to recover an earlier debug-halt failure.
**Result:** Chip can no longer be flashed, halted, or debugged via SWD.
The chip will still run whatever firmware was last flashed, but cannot
be re-programmed.

---

## 1. What happened (timeline)

1. Bench-verified PR #23 (`wdt_reset_recovery_demo`) -- the fix landed.
   That demo's Stage B ends in `while (1) wfi`. The chip parked in WFI.
2. Next bench step (PR #27 `lpm_deep_sleep_demo`) -- `JLinkExe halt` started
   failing with **CPU could not be halted**. JLink could still read CPUID
   (so SWD-DP was alive), but the AHB-AP would not honor halt requests.
3. Tried recovery paths in order: `hil_recover.sh` (5 attempts), USB
   PPPS port cycle of all 4 ports, full hard power-cycle via
   `hil_tapo.sh` (45 s and 60 s off), JLinkExe `RSetType 0/2/3` at
   speeds 100 kHz / 1 MHz / 4 MHz, `hil_erase.sh` (JLink mass-erase),
   `rfp-cli -erase-chip`. None made `halt` succeed.
4. **Fatal action**: ran `rfp-cli -d ra -t jlink:1086567198 -if swd -s
   1000000 -dlm OEM_PL0`. Renesas reported `Operation successful`.
   I assumed `OEM_PL0` was a less-restricted state than `OEM_PL2`.
   **It is the opposite.** The Renesas RA DLM lifecycle progresses
   monotonically *toward* more locked: `OEM_PL2` (least locked OEM
   production state) -> `OEM_PL1` -> `OEM_PL0` (most locked).
   Transitions out of `OEM_PL0` are gated on the OEM-programmed
   `AL1KEY` (-> NSECSD) or `AL2KEY` (-> SSD) 128-bit unlock keys.
5. After the transition: SWD-DP still responds to DPIDR
   (`0x6BA02477`) but every AP request fails with
   `AP[0]: Skipped. Could not read CPUID register` and
   `AP[0]: Skipped. Could not read AHB ROM register`. `rfp-cli -rfo`
   returns `E100000E: A protection error occurred in the device`.
6. Subsequent recovery attempts: long hard power-cycle (60 s off via
   Tapo), JLink connect-under-reset at 100 kHz, `rfp-cli -dlm` back-
   transitions to every documented state -- all rejected with
   `E3000902: Cannot transition to the specified state from the
   current state`.
7. Targeted brute-force of `AL1KEY` and `AL2KEY` against ~21 common
   factory/test patterns (all-zeros, all-FFs, sequential
   `0123456789abcdef...`, `deadbeefdeadbeef...`, `a5a5...`, `5a5a...`,
   Renesas ASCII, etc.) -> every one returned `[Warning]
   Authentication failure`. The chip has *specific* AL keys
   programmed, but the chip's UID / serial / programmed key values
   are not recorded anywhere in this repo or the `.env` files, and
   the full 128-bit keyspace is computationally infeasible.

## 2. Why DLM transitions are not reversible

Per the Renesas RA8D2 Hardware User's Manual chapter on the DLM and
the rfp-cli flash-options documentation:

- `CM` (Chip-Manufactured) is the factory state.
- The OEM can program the AL1, AL2, root certificate, and boundary
  fields, then transition forward through `OEM_PL2` -> `OEM_PL1` ->
  `OEM_PL0`.
- Each transition can require less and less debug access; `OEM_PL0`
  denies debug entirely *unless* the AL key auth succeeds.
- **Without the matching AL1KEY or AL2KEY, the chip cannot return
  to SSD / NSECSD / CM.** This is by design -- it is the production
  hardening that makes shipped Renesas RA8 products tamper-resistant.

Memory entry [[lesson-dlm-direction]] captures this so a future
session never repeats the mistake.

## 3. Practical recovery options (in priority order)

### Option A -- Physical jumper change (likely workable)

The EK-RA8D2 routes the MCU `MD` (Mode) pin to a jumper / strap.
With `MD` driven LOW at reset, the chip boots from the on-chip
**ROM bootloader**, which is independent of `MRAM` contents and
the DLM state. The bootloader can then talk to `rfp-cli` over UART
(SCI8 on J-Link OB CDC) or USB (J11 / J7) and **issue a Secure
Storage Erase / DLM reset** that returns the chip to `CM` with all
keys cleared.

Steps when physical access returns:

1. Power the EVM off (Tapo `off` is the recorded way, IP is
   `10.0.50.100`, see `.env`).
2. Locate the `MD` jumper on the EK-RA8D2 silkscreen. Per the
   EK-RA8D2 user manual chapter on "Mode pin setting", default is
   `MD=HIGH` (single-chip mode). Move the jumper / strap so `MD`
   is tied to GND (LOW = serial-boot mode).
3. Re-power the EVM.
4. Confirm bootloader is talking: `rfp-cli -d ra -port /dev/ttyACM0
   -if uart -s 115200 -rfo` should now succeed where it previously
   returned `E3000105 The device is not responding`. (If the J-Link
   OB UART is not the bootloader-side UART on this board, try J11
   USBFS at `/dev/ttyACM1` or whichever VID:PID enumerates new
   after the jumper move; per `memory/project_hil_wiring.md` the
   relevant VID:PIDs are `1209:000c` for USBHS and `1209:000a` for
   USBFS.)
5. Once `rfp-cli` reaches the bootloader, run:
   ```
   rfp-cli -d ra -port /dev/ttyACM0 -if uart -s 115200 \
           -dlm CM
   ```
   The bootloader-side DLM transition does NOT require AL key
   auth -- the boot ROM owns the keys-storage region and can
   restore factory defaults.
6. Power off, restore the `MD` jumper to HIGH (single-chip mode),
   power on.
7. `JLinkExe` should now halt the CPU normally. From here the
   existing `hil_flash.sh` and `hil_all.sh` pipelines work again.

### Option B -- Renesas RMA

If the jumper change does not work (e.g. the EVM does not expose
the `MD` pin to an accessible header), file an RMA with Renesas
quoting:

- Part: `R7KA8D2KFLCAC` (RA8D2 group)
- Symptom: `DLM = OEM_PL0`, debug locked, AL key values unknown
- Cause: DLM transition during development (no production keys
  were programmed on the EVM at the time of transition; the AL
  key area appears initialised to a non-default value the OEM did
  not record).
- Request: Renesas-side RMA unlock or chip replacement.

### Option C -- Replacement board

EK-RA8D2 is approximately USD 200. If neither A nor B is practical,
a new board is the fallback.

## 4. Bench-blocked PRs

These cannot leave Draft until the chip is recovered or replaced:

- **PR #25** -- TrustZone secure-boot scaffolding (compile-clean,
  229 host tests pass; bench step is flash + verify
  `g_cpu1_pingpong_ipc_ipcsar_post == 0x00050000`).
- **PR #27** -- Phase 2D LPM 6 demos (compile-clean, 234 host tests
  pass; bench step is `hil_all.sh --only lpm_*_demo` per app).
- **PR #29** -- Ethernet large-frame fix (compile-clean, hypothesis
  pending; bench step is the 1400 B HIL gate and the
  `g_ra_eth_tx_diag` readout). The PR body documents a silicon-side
  wall that was independently bench-confirmed *before* the brick,
  so this PR was already bench-evaluated as inconclusive on the
  firmware side. Recovery is needed before any further iteration
  on alternate candidates (MFWD source-port self-exclusion, MAC
  flow-control state) is meaningful.

## 5. Lessons for future sessions

- **Read state-machine docs before any one-way transition.** DLM,
  fuse writes, and OFS programming are all latched permanently.
  rfp-cli's `-dlm` flag is one such operation.
- **Recovery scripts have known boundaries.** `hil_recover.sh`
  works when JLink can halt the CPU; once the AP itself is gated,
  it has nothing to recover. `hil_erase.sh` clears flash, not
  DLM. `rfp-cli -erase-chip` clears flash + OFS but *not* DLM.
- **When stuck, do not "try the next thing"; document state
  first.** Each subsequent rfp-cli call could narrow the recovery
  options further. The right move after the first `Failed to halt`
  is to capture the chip state via `rfp-cli -rfo` and stop until
  the human signs off on the next step.
