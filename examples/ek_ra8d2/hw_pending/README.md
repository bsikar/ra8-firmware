# examples/ek_ra8d2/hw_pending/

Apps here compile and pass all CI gates but have **not yet been confirmed
working on hardware end-to-end** -- each is blocked by a peripheral that
isn't on the bench, or by a documented firmware gap. These are the apps to
work through next when the missing hardware is available.

To build: `make <appname>` from the repo root, e.g. `make sd_font_render`.
Move an app to `hw_validated/hil/` (with a `git mv`) once it passes a
hardware HIL probe AND the gap below is resolved.

## Apps and blocking reasons (current as of 2026-06-20)

### Blocked by a peripheral not on the bench

| App | Blocking reason |
|-----|-----------------|
| i3c_i2c_peripheral_demo | I2C/I3C peripheral mode needs an external controller to talk to; the bench has no controller wired up. |
| imu_lsm6dso_demo | Needs an LSM6DSO IMU on the I2C/I3C bus; not fitted. |
| manual/usb_msc_sdcard | Serves the live full-capacity Pmod2 SD card as a WRITABLE USB drive (#206 items 1-2). `scripts/sim/smoke.sh usb_msc_sdcard` proves the transport headlessly (enumeration + READ CAPACITY equal to the card image's block count + a 512-byte sector-0 read through the USB pipe), but #206 item 3 -- a real PC host copying a file on, unmounting, re-mounting, and verifying (multi-block WRITE(10) streaks, flush semantics, FS metadata updates) -- needs a human at a PC on J11; see the app README's bench procedure. |

### Blocked by a module / firmware gap

| App | Blocking reason |
|-----|-----------------|
| tz_nsc_cgc_usb | Needs the real TrustZone secure / non-secure partition (separate S + NS stacks/.bss and a proper BLXNS transition): the NS-pointer veneer check rejects args whose `.data`/`.bss`/stack live in secure SRAM. Tracked in #60 / #54. |
| dtc_transfer_demo | `tools/board_sim` shadows the DTC control window (DTCCR/DTCVBR/DTCST/DTCSTS) but models no descriptor-table transfer engine, so the SRAM-to-SRAM copy reports `match=N` on the emulator and can only be confirmed on silicon. The TI encoding + indirect vector-table model + slot re-arm are host-tested; the bring-up + ELC/DTC activation run headless. Tracked in #124. |
| lvd_monitor_demo | `tools/board_sim` shadows the PVD control window but models no analog voltage comparator, so `PVD1SR.MON` reads back 0 and the banner reports `mon=below ok=N` on the emulator; the live `mon=above` reading needs silicon. The verdict logic + status decode + PRCR.PRC3 unlock word are host-tested; the safe flags-only (`response=none`) bring-up runs headless. Tracked in #125. |
| bkup_survival_demo | The VBTBKRn read/write window passes on `tools/board_sim` (`rw=ok`), but the emulator models no reset-retained VBATT power domain and clears memory each run, so it reports `survived=N boot=1` -- the reset-survival proof needs a real EK-RA8D2 and a reset cycle (boot1 `survived=N` -> reset -> boot2 `survived=Y`). The pattern + verdict + survival decision are host-tested. Tracked in #131. |
| cac_accuracy_demo | `tools/board_sim` shadows the CAC control registers but models no edge counter, so `CASTR.MENDF` never asserts and `ra8_cac_measure` returns `k_ra8_err_hw_timeout` (`meas=TIMEOUT ok=N`). Measuring the 24 MHz main osc against LOCO/32 (~23437 counts, +/-6% window) needs silicon. The expected-count + window math, the verdict (MC/DC), and the CACR1/CACR2 clock-select are host-tested. Tracked in #126. |
| pdg_delay_demo | The PDG bring-up + delay-program + read-back path runs headless on `tools/board_sim` (`cfg=ok`, registers take the config), but the PDG has no software-readable "edge was delayed" status -- its only effect is the *timing* of a GPT output edge, which needs a running GPT32_0 PWM source + a scope / logic analyzer to measure. That delay measurement is the real acceptance, so the app stays hw_pending. The delay-code range + the configured verdict (MC/DC) are host-tested. Tracked in #132. |
| drw_fill_demo | `tools/board_sim` shadows the DRW (2D Dave engine) control registers and `ra8_drw_wait_idle` returns, but it models no rasterizer, so `ra8_drw_fill_rect` never touches the framebuffer and the centre pixel stays clear (`match=N`). The actual fill needs silicon. The pixel-index math + the fill verdict (MC/DC, against a software fill model) are host-tested. Tracked in #120. |
| ecc_monitor_demo | Enables full SRAM ECC + zero-init on spare bank 2 and round-trips an ECC-protected buffer; `tools/board_sim` shadows `SRAMCRn`/`SRAMESR` so the bring-up runs (`ok=Y`), but it does **not** model ECC, so real error detection (a 1-bit/2-bit flip setting `SRAMESR`) can't be exercised -- the verdict gates only on the deterministic round-trip and reports the error masks via `g_ecc_1bit`/`g_ecc_2bit` for the bench. Error injection is the real acceptance. Tracked in #130. |
| dotf_selftest_demo | Brings up the DOTF (Decryption On The Fly) block and runs its AES built-in self-test (`REG00` bit 20) on both channels -- key-free, no channel armed, no persistent writes. `tools/board_sim` does **not** model the DOTF APB window (`0x4026_8800`/`0x4026_8900`), so the self-test register reads are not a real AES core: the bring-up + self-test calls return `k_ra8_ok` and the demo reports `ok=Y`, but the genuine AES BIST result + the in-place decryption path only exist on silicon (the opaque `REG00` snapshots go to `g_dotf_st0_snap`/`g_dotf_st1_snap`). The verdict (MC/DC) is host-tested. The on-silicon self-test result is the real acceptance. Tracked in #127. |
| ra8_sdhi_card_demo | **Raw-HAL SDHI block smoke test -- no ra8_io, no ra8_fs.** Drives the on-board microSD through the native SDHI 4-bit controller directly via `ra8_sdcard` / `ra8_sdhi`, then writes + reads back one raw 512-byte block (LBA 64) and byte-compares. `tools/board_sim` models the SDHI host controller (`board_periph_sdhi.c`) so the app reaches its PASS banner headlessly (`ra8_sdhi_card_demo: native SDHI block round-trip PASS`). The bench run on real silicon confirms the HAL without any file-system layer. Tracked in #123. |

## Recently promoted

The on-board **USB self-loop self-tests** (`usb_selftest_hs_host`, `_fs_host`,
`_cdc`, `_hid`, `_microsd`, `_mlun`, `_wlun`, `_ospi`, `_ospi_rw`, `_soak`) were
validated on real hardware (FS jack cabled to HS jack, the board both hosts and
devices itself) and moved to `hw_validated/hil/`.

`sd_font_render` was reworked to **self-provision** its font through the new
`libs/ra8_sdfont` helper: it mounts the Pmod2 microSD, and if `FONT.OTF` is absent
it writes a baked Latin-1 font (`libs/fonts/literata_latin1.ttf`) to the card and
reads it back -- so any FAT-formatted "random" card just works, no host-side
image prep. Validated on real hardware with the prior Arno Pro face
(`g_sfr_stage` = render_ok, font 404 KB, `g_sfr_ink` = 1254 inked pixels) and in
board_sim against a blank card image (`mkfontimg --blank`), gated by a
`g_sfr_heartbeat` memprobe, and moved to `hw_validated/hil/`; re-validation with
Literata is a hardware follow-up. The same helper now backs `ereader_ui`'s font
load.

`tz_secure_only_sd` was also validated -- a real SPI-mode microSD round-trip
(SCI0 Simple-SPI -> `ra8_sdmmc_spi` -> `ra8_fs`: init, mount, write+read+compare,
`sd: roundtrip ok`) on the Pmod2 card -- and moved to `hw_validated/hil/` with a
`uart_scrape` gate.

`threadx_filex_demo` was rewritten to run FileX over the on-board OSPI flash
(LevelX) instead of the unreachable SDHI card path, validated on real hardware
(format -> FAT -> create/list/read-verify/delete -> `ospi FAT roundtrip ok`), and
moved to `hw_validated/hil/`.

`usb_host_msc_browse` was re-based onto the self-loop (board hosts AND simulates
the MSC peripheral over the J7<->J11 cable, no external drive): the host
enumerates, mounts, and BROWSES the device's FAT root before read-verify
(`USB HOST MSC BROWSE PASS`), and moved to `hw_validated/hil/`.

`usb_host_keyboard` was likewise re-based onto the self-loop: the board simulates
a boot-keyboard device and the host decodes its keycodes back to "RA8D2"
(`USB HOST KEYBOARD PASS`), and moved to `hw_validated/hil/`.

`dfu_bootloader` was **fully bench-validated** end-to-end and moved to
`hw_validated/hil/`. The "needs an external dfu-util" gap is closed: the board is
its own DFU host over the J7<->J11 self-loop via
[`dfu_selftest_boot`](../hw_validated/hil/dfu_selftest_boot), which DFU-flashes +
commits a bootable Slot A; flashing the bootloader afterward (Slot A untouched)
then boots it (J-Link sentinel `0x600D600D` @ `0x22040000`). The bootloader itself
is gated in HIL with an `alive` probe (boots + runs its A/B boot decision without
faulting -- PC in code, CycleCnt advancing, CFSR/HFSR clean), deterministic
regardless of slot state, so it needs no human in the loop.

## Retired

`usb_host_cdc_echo` was **deleted**: its board-only function (a CDC host
enumerating + byte-echoing a CDC-ACM device) is already fully validated by
`hw_validated/hil/usb_selftest_cdc` (the CDC host+device self-loop). The only
things it additionally exercised -- a *real external* CDC peripheral and the
interrupt-driven host path -- need external hardware on J7 plus the USB-host
ICU/NVIC IRQ wiring (#62), so keeping a redundant self-loop duplicate added no
coverage. Its host test and its SRS/SVCP/roadmap entries were removed with it.
