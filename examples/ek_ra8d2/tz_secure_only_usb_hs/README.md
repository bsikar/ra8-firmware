# tz_secure_only_usb_hs

TrustZone diagnostic experiment. This app is a copy of `usb_cdc_echo`
with **every TrustZone artifact stripped out**:

- `trustzone_init.{c,h}` is a no-op (returns immediately, no SAU
  programming, no IDAU touching).
- `secure_exception.c` is empty -- the chip's default handler
  (weak alias to `Default_Handler`, which traps to `bkpt #0`)
  catches any unexpected fault.
- `linker_script.ld` removes the `.gnu.sgstubs` veneer section and
  the `NS_MRAM` / `NS_SRAM` placeholder regions; every section
  lands in the lower (Secure) half of MRAM (`0x02000000`-`0x0207FFFF`),
  SRAM (`0x22000000`-`0x220FFFFF`), and SDRAM (`0x68000000`-`0x69FFFFFF`).
- `system_init.c` does not touch the SAU/IDAU registers (it never did
  in the dual-world build either, but the call to `ra_trustzone_init`
  is still gated out here).
- `CMakeLists.txt` forces `RA_TRUSTZONE_ENABLE=OFF` regardless of the
  top-level cache, so `-mcmse` is never on the command line.

## Why this experiment exists

On real EK-RA8D2 silicon the dual-world `usb_cdc_echo` build panics
during boot because `ra_cgc_pll2_enable` returns `k_ra_err_hw_timeout`.
The PRCR-protected register writes to PLL2CR / PLL2CCR / PLL2CCR2 /
USBCKCR / USBCKDIVCR (HUM Ch. 9 "Clock Generation Circuit") are
silently dropped. JLink-direct PRCR-unlock + PLL2CR writes from the
debugger work fine, which rules out a hardware fault and points at a
software-level scope/protection issue.

The hypothesis: the chip boots into the Non-Secure world (or the SAU
partition labels the CGC peripheral window Non-Secure), so PRCR
unlocks issued from the firmware's address aliases never arm the
PLL2 register window.

This app verifies whether running entirely in Secure mode -- no SAU
partition, no NSC veneer, IDAU at reset state -- lets
`ra_cgc_pll2_enable` succeed on real silicon and USB-FS enumerate.

## What to look for on the bench

A `printf` line goes out on SCI8 / J-Link RTT at the very first
instruction of `main()`:

```
[TZSECONLY] tz_secure_only_usb_hs boot, attempting CGC PLL2 enable
```

- See the line **and** the host enumerates `/dev/cu.usbmodem*` -> the
  TrustZone hypothesis is correct; the dual-world build needs a
  proper Secure-side CGC bring-up before any NS code runs.
- See the line but no enumeration -> PLL2 still failed; it is not the
  Secure/NS split alone.
- Don't see the line at all -> the boot scaffolding never reached
  `main()`; the regression is in `Reset_Handler` / `SystemInit`, not
  CGC.

## Pinout, VID/PID, descriptors

Identical to `usb_cdc_echo`. See
[`../usb_cdc_echo/README.md`](../usb_cdc_echo/README.md) for the wiring
table, VID/PID, and CDC ACM descriptor commentary.

## Build / flash

```
cd examples/ek_ra8d2/tz_secure_only_usb_hs
make             # -> build/tz_secure_only_usb_hs.elf / .hex / .bin
make flash       # JLinkExe load via scripts/flash.sh
make ozone       # SEGGER Ozone GUI
```
