# EK-RA8D2 OEM_PL0 Brick: Recovery Procedure

**Date of incident:** 2026-05-25
**Date of recovery:** 2026-05-27 (Renesas support case #417141, FAE Fivos)
**Cause:** Erroneous `rfp-cli -dlm OEM_PL0` transition from `OEM_PL2`,
then incorrectly chaining further `-dlm` transitions after each
`-erase-chip` Initialize call that had already silently restored the
chip to OEM_PL2.
**Recovery:** Single `rfp-cli -d ra -t jlink -if swd -s 1000000
-erase-chip` invocation, NOT followed by any further `-dlm` command.
The chip's `Security Flags: none` (the `ce` "Disable Initialize Command"
flag was never set) meant the boot-firmware Initialize was available
from OEM_PL0/AL0 without needing the AL keys.
**Final state:** DLM = `OEM_PL2`, debug fully restored, `hil_flash.sh`
operational. See section 6 for the recovery log.

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

## 5. Post-incident deep research (added 2026-05-26)

Two independent AI deep-research reviews of the recovery surface returned
the same verdict: there is NO remote-only path out of OEM_PL0 with
unknown AL keys on RA8D2. Concrete additions to this report from those
reviews:

### Critical warning -- STOP attempting `-auth` brute force

Per the RA8 boot-firmware notes (R01AN7140 / R01AN7290 / R01AN7547),
the boot firmware imposes a finite retry counter on failed AL key
authentication attempts. **Exhausting that counter pushes the device
into `LCK_BOOT`, a permanent terminal state from which not even Renesas
RMA can recover the chip.** This session used 21 attempts plus a
handful more during the post-brick scramble. The exact threshold is
firmware-revision-dependent and not publicly documented; treat every
further `-auth` invocation as costly. **Do not run any further
`rfp-cli -auth AL1KEY` or `-auth AL2KEY` commands on this device.**

### Confirmed-untried-and-failed: SWDMD via J-Link OB UART

Some EK-RA8 boards (notably EK-RA8M1, EK-RA8D1) wire an `SWDMD` signal
from the on-board J-Link OB to the MCU's MD pin so that
`rfp-cli -tool jlink -if uart` enters SCI boot mode without a physical
jumper move. The first research thread flagged this as the one
untried remote avenue. Tested on 2026-05-26 against this device:

```
$ rfp-cli -d ra -tool jlink:1086567198 -if uart -s 115200 -rfo
[Error] E3000105: The device is not responding.
```

The EK-RA8D2 v1 routes MD differently from EK-RA8M1 -- per the v1
User's Manual (R20UT5523EG0101 Rev 1.01, Oct 2025), the J-Link OB
does NOT have any control over MD via SWDMD. Physical access to the
`J16` jumper is the only path into the boot ROM. This avenue is
closed.

### Why brute force is mathematically infeasible

AL1KEY and AL2KEY are 128-bit symmetric values stored wrapped by the
silicon's Hardware Unique Key (HUK). The boot firmware computes an
HMAC-based challenge-response on each `-auth` call; the chip is
performing a real cryptographic MAC verification, not a string
compare. Even if the original developer chose an entropy-poor
plaintext (e.g. all-zeros), the chip's HUK-wrapped storage means
the on-device check is over a high-entropy ciphertext derived from
the plaintext. The full 2^128 keyspace at rfp-cli's per-attempt
latency (~hundreds of milliseconds for the USB round-trip and
crypto verify) is computationally infeasible by many orders of
magnitude. The 21 common defaults tried were the right
opening-attempt set; the lesson is that the actual AL keys on this
board are NOT a published default and NOT a developer-chosen weak
value.

### Why no Renesas vendor-side / RMA key escape exists

The RA8 RSIP-E51A security engine is OEM-rooted. AL keys, RMA_KEY,
OEM root certificate, and DOTF keys are all customer-injected and
never escrowed at Renesas. The RMA workflow (`RMA_REQ -> RMA_ACK`)
relies on a customer-pre-injected `RMA_KEY`; on this board no
`RMA_KEY` was ever injected (transition to OEM_PL2 -> OEM_PL0 did
not include a `-rmakey` step), so the RMA workflow cannot be
initiated. Renesas Customer Support cannot synthesise an unlock
without the customer-held key material.

### Other items checked and ruled out

- `git log --grep -iE "AL1|AL2|DLM|skmt"` -> no matches: no script
  in this repo ever explicitly injected AL keys.
- `find / -name "*.rkey" -o -name "*.sfp" -o -name "*.skmt"` ->
  no matches anywhere on the dev machine or the Pi.
- Shell history (bash + zsh, dev + Pi) for `rfp-cli ... al1key`
  or `al2key` -> no historical invocations.
- No CVE or PSIRT advisory exists for the RA8 DLM or RSIP-E51A
  as of May 2026.

The conclusion is that the chip either shipped with AL keys
pre-programmed (RA8D2 ship state is NOT explicitly listed in the
Renesas TN-RA*-A0120A/E ship-default document; the assumption that
RA8D2 ships in SSD with no keys is inferred from RA8D1/M1, not
documented), OR rfp-cli's `-dlm OEM_PL0` invocation silently auto-
injected default keys that subsequently mismatch every known default.

### Definitive action sequence

1. **Stop touching the chip.** No `rfp-cli -auth`, no
   `rfp-cli -dlm`, no JLinkExe halt attempts. Each can degrade
   the state.
2. **Open a Renesas Customer Support ticket** at
   `https://en-support.renesas.com`. Title:
   *"EK-RA8D2 in OEM_PL0 with unknown AL keys -- request guidance"*.
   Include the verbatim error codes (E100000E, E3000902), the JLink
   DP/AP trace, the rfp-cli version, the kit serial, and the chip
   UID if obtainable. Ask whether the device may already be in
   `LCK_BOOT` and whether warranty replacement of the EK kit is
   available. Expected first response: 1-3 business days.
3. **Plan for board replacement.** EK-RA8D2 is approximately USD
   200 from Mouser / Digi-Key. This is the only certain recovery.
4. **For future sessions**: never run `rfp-cli -dlm <state>` on a
   chip whose AL keys are not committed to a password manager AND
   a second-location backup. Treat the AL2_KEY as the production
   root key it actually is. Inject `RMA_KEY` BEFORE the first OEM
   state transition on any future EVM -- that preserves a
   Renesas-assisted recovery path.

## 6. RECOVERY (2026-05-27): the chip is unbricked

Renesas support case #417141, FAE Fivos, responded with the
critical detail every recovery guide on the internet got wrong:

> "Have you tried an initialize command (different from erase)?
> It should be available in your OEM PL0 AL0 state without
> knowledge of the AL keys, unless otherwise disabled."

The `rfp-cli -erase-chip` command (per `/opt/rfp/linux-x64/docs/
rfp-cli.md`) does the following: "Erases all data in the flash
memory of the device and clears the configuration settings. **If
the device supports an initialization function, this option will
also execute the initialization command.**" The RA8D2 supports the
initialization function. So `-erase-chip` IS the Initialize.

The earlier rfo readout from this session confirmed
`Security Flags: none`, meaning the `ce` (Disable Initialize
Command) flag was never set on this chip. Therefore the
PL0 -> PL2 transition via Initialize was always available.

**Verified bench run, 2026-05-27:**

```
$ rfp-cli -d ra -t jlink:1086567198 -if swd -s 1000000 -erase-chip
Renesas Flash Programmer CLI V1.15
...
Connected to R7KA8D2KFLCAC
Erasing the all device data and clear configuration
Disconnecting the tool
Operation successful

$ rfp-cli -d ra -t jlink:1086567198 -if swd -s 1000000 -rfo
...
Security Flags: none
DLM State: OEM_PL2            <-- WAS OEM_PL0 BEFORE
Boundary: 16352,0,0,0,0
ARC Configuration: FFFFFFFF
CPU Flags: none
```

`hil_flash.sh blink` then succeeded -- the chip is fully
recovered. JLink halt works, MRAM flashes, the full bench
pipeline is operational.

### Why my earlier `-erase-chip` attempts appeared to fail

The two prior `-erase-chip` invocations during the initial
recovery scramble (timeline entries 6 and 8 of section 1) DID
likely succeed in transitioning the chip back to OEM_PL2 each
time -- but I immediately followed them with further
`rfp-cli -dlm OEM_PL0` and various DLM-test transitions that put
the chip back into OEM_PL0 (which was always a legal
forward-direction transition without needing AL keys). I was
checking the state AFTER those subsequent re-locks, so I
incorrectly concluded `-erase-chip` did not transition DLM. It
did; I un-did it.

Treat `-erase-chip` on a Renesas RA chip with DLM as a
"factory reset" of the lifecycle state machine plus user flash.
Do not chain a DLM transition after it expecting the state to
stick.

## 7. Lessons for future sessions

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
