# dfu_bootloader

The immutable first 128 KiB of code-MRAM. It never erases itself. At every reset
it validates both application slots and the persistent `boot_state` metadata,
boots the active or trial slot via copy-to-run, or enters a USB-FS DFU device
to program the inactive slot. Built on the controller-agnostic `libs/ra8_dfu`
core shared with `dfu_selftest_*` and `ra8_rot`.

## Bank layout (1 MiB MRAM)

```
0x02000000  bootloader  128 KiB  (never erased by DFU)
0x02020000  Slot A      444 KiB  [image body | 32-byte header in the last page]
0x0208F000  Slot B      444 KiB  [image body | 32-byte header in the last page]
0x020FE000  boot_state    8 KiB  [32B copy0 @0x020FE000 | 32B copy1 @0x020FE020]
0x02100000  (end)
```

Each slot's 32-byte header (`ra8_dfu_hdr_t`, magic `"RA8D"`) lives at the
**last** 32-byte page of its slot (`slot_base + 0x0006EFE0`, max payload body
`0x0006EFE0` = 454,624 bytes) and is programmed **last** so a torn download
never leaves a half-valid image.

The top 8 KiB (`0x020FE000..0x02100000`) holds two alternating 32-byte
`ra8_dfu_boot_state_t` ping-pong pages (`copy0` at `0x020FE000`, `copy1` at
`0x020FE020`), each protected by an IEEE CRC32 over bytes `[0..28)`. Every update
erases and programs only the inactive copy page and increments `write_seq`, so
losing power mid-update leaves the previously valid copy intact.

## Boot decision, trial boot, and revert

Boot decision and state arbitration live in `libs/ra8_dfu` (`ra8_dfu_boot_decide_trial`,
`ra8_dfu_boot_state_pick`) with MC/DC unit tests in
`tests/misc/src/test_ra8_dfu_boot.c` and `libs/ra8_dfu/tests/`.

1. **DFU trigger / no valid slot:** Reads and clears the one-shot no-init SRAM
   trigger (`0xDF00B007`), then validates both slots (header magic, length,
   CRC32, and entry check). If the trigger was set or neither slot is valid,
   enters USB-FS DFU targeting the inactive slot (or Slot A).
2. **Confirmed or untracked boot:** Picks the valid slot with the higher
   sequence number (Slot A on tie). If `boot_state` already marks that slot
   confirmed (`confirmed == 1`) or no trial state tracks it, launches it directly
   with WDT0 disarmed.
3. **Trial boot:** After a USB-DFU commit, `blc_device_worker` writes
   `boot_state` for the newly programmed slot with `confirmed = 0` and
   `tries_left = k_ra8_dfu_boot_state_default_tries` (`3`) before resetting.
   While `preferred` remains unconfirmed and `tries_left > 0`:
   - `dfu_bootloader` persists `tries_left - 1` into the alternate ping-pong page
     **before** launching so even an instant fault consumes a try.
   - `dfu_bootloader` arms WDT0 (`blc_arm_trial_watchdog_or_halt()`) in
     reset-on-expiry mode (`k_ra8_wdt_timeout_1024`, `k_ra8_wdt_clkdiv_4`,
     ~25.6 s timeout, `RSTIRQS = 1`, `SLCSTP = 0` so WFI still counts down).
   - It then verifies the image trailer (`ra8_rot_verify_image`) and launches at
     `k_ra8_dfu_run_base`.
4. **Revert on exhausted tries:** If a trial image hangs, faults, or reboots
   without calling `ra8_dfu_confirm()`, WDT0 resets the MCU (`RSTSR1.WDTRF`).
   Once `tries_left == 0` with `confirmed == 0`, the next reset skips `preferred`
   and falls back to the alternate valid slot (non-trial, WDT0 disarmed), or
   enters DFU mode if no valid fallback exists.

Note on anti-rollback: unconfirmed trial launches route through
`ra8_dfu_launch_trial()`, which verifies `img_version >= stored_min` without
advancing Extra-MRAM (`0x02E07600`) prior to confirmation; only confirmed boots
(`ra8_dfu_launch()`) advance the monotonic floor.

## Unified pipeline contract alignment

Annotated against `docs/design/unified_update_pipeline.md`
(`transport = dfu`, `apply_mode = ab_slots`, `trial_budget = 3`,
`defer_floor_bump = true`):

- **Agrees:** Writes 32-byte pages into the inactive slot, verifies SHA-256 +
  ECDSA-P256 (`ra8_rot_verify_image`) + anti-rollback before branching to SRAM
  run base, and isolates unverified payloads away from the active image.
- **Diverges:** Uses USB-FS DFU push ingestion + header-last (`"RA8D"`) slot
  commit followed by reset-time `ROT1` verification and WDT0 trial/revert in
  `{World: S}`, rather than in-app pull verification over an `"RA8U"` N-image
  bundle with `{World: NSC}` bank-swap veneers (`ra8_ota`).

Because the 128 KiB bootloader itself is never modified by DFU, an SWD re-flash
always recovers the device regardless of slot or metadata contents.

## What a payload must do to stay live

To run across both confirmed and trial boots without reverting:

1. **Link at `k_ra8_dfu_run_base` (`0x22020000`):** Every slot is copied to SRAM
   before execution (see "One image, either slot" below). Any MRAM-writing code
   (including `ra8_dfu_confirm()`) must execute from SRAM. See
   `payload/confirm_payload.c`, `payload/confirm_payload.ld`, and
   `scripts/build_confirm_payload.sh` for a freestanding minimal reference image
   that links `libra8_dfu_boot.a`, `libra8_hal.a`, `libra8_core.a`, and
   `ra8_flash.c`.
2. **Initialize or preserve flash/MRAM access and run self-test:** Call
   `ra8_flash_init()` and run post-boot health checks (clock, comms, peripheral
   sanity) before confirming.
3. **Feed or reset past the ~25.6 s trial watchdog:** On a trial boot, WDT0 is
   latched until reset (HUM Ch 27 write-once `WDTCR`/`WDTRCR`). If startup plus
   self-test takes close to ~25.6 s, call `ra8_wdt_refresh()` /
   `ra8_wdt_refresh_for(k_ra8_wdt0)`. Once confirmed, either continue periodic
   WDT0 refreshes for the remainder of that boot or trigger a clean software
   reset so the next boot starts in confirmed mode with WDT0 disarmed.
4. **Call `ra8_dfu_confirm()`:** Marks `confirmed = 1` in the ping-pong MRAM
   record (`k_ra8_ok`). `ra8_dfu_confirm()` is idempotent and accepts
   `tries_left == 0` if called during the final trial attempt before reset.

## One image, either slot (copy-to-run)

A slot is staging only. On boot the bootloader copies `img_len` body bytes to
`k_ra8_dfu_run_base` (`0x22020000`) and branches there via `ra8_dfu_launch`, so
one payload binary runs identically from Slot A or Slot B without position-
independent relocation (`ROPI`/`RWPI` breaks `.rodata` interface vtables). The
header records `entry`, but the bootloader copies only to `k_ra8_dfu_run_base`
and drops to DFU if `ra8_dfu_run_target_valid` rejects the header.

## Staging and emulator test scripts

- `scripts/stage_slot_image.py` stages a plain or `rot_sign.py`-signed binary
  into a slot Intel HEX file at Slot A (`0x02020000`) or Slot B (`0x0208F000`).
- `scripts/build_confirm_payload.sh` compiles the reference self-testing and
  confirming payload (`payload/confirm_payload.c`).
- `scripts/trial_boot_emu_test.py` stages Slot A (`seq=1`) + an unconfirmed
  hanging Slot B (`seq=2`) + initial `boot_state` (`0x020FE000`) and drives
  `ra8_emulator` (`--mode watchdog-reset`, `--mode revert`, `--mode selftest`)
  to verify WDT0 arming, reset via `WDTRF`, try exhaustion, and revert to Slot A.
