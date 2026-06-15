# tz_nsc_cgc_usb (hw_pending)

Non-Secure variant of `usb_cdc_echo` that proves the three NSC
veneers (`ra_nsc_cgc_pll2_enable`, `ra_nsc_cgc_usbfs_clock_enable`,
`ra_nsc_cgc_get_clock_hz`) really do trap into the Secure world
and forward to the underlying `ra_cgc_*` drivers.

## Status (2026-05-19)

Newer probing pinned the failure precisely:

- `g_tz_nsc_cgc_usb_init_step` halts at **1** -- meaning the very
  first call, `ra_nsc_cgc_pll2_enable`, returns non-OK and the
  demo hits `demo_panic_halt()` immediately. The chip never gets
  to USBX bring-up, so no enumeration happens.
- The Secure side's `ra_cgc_pll2_enable` works fine
  (`tz_secure_only_usb_fs` calls the equivalent function from the
  Secure world and the chip enumerates as 1209:000a). The bug is
  on the NSC bridge, not the Secure-side driver.

Likely root causes:

1. The NSC veneer table in `libs/ra_nsc/` isn't being linked into
   the Secure-side image, so the NS-side call lands on a stub
   that returns `k_ra_err_not_supported`.
2. The SAU configuration doesn't route the veneer-page address
   alias (the 0x10000000 NSC veneer alias defined in
   `trustzone_init.c`) to the actual veneer code in MRAM. The NS
   call resolves to an undefined branch and the cmse_nonsecure
   entry returns an error word.
3. The Secure-side state isn't in the right mode at the time of
   the veneer call (e.g. PRCR locked, MSTPB clock-gate set).

## What landed this turn

The USB-descriptor fixes that unblocked `usb_cdc_echo` and
`threadx_usbx_cdc_demo` have been applied to this main.c too --
once the NSC bring-up is correct the demo will enumerate as
1209:000a immediately:

- Device descriptor bcdUSB = 0x0200 (was 0x0110: USB 1.1 is
  silently rejected by macOS for IAD-based composite devices).
- Configuration descriptor wTotalLength = 0x4B / 75 bytes (was
  0x43 / 67 bytes: dropped EP1 IN, causing USBX to deref a NULL
  endpoint after SET_CONFIG).
- bmAttributes = 0x80 (bus-powered) instead of 0xC0
  (self-powered with bMaxPower=100mA -- a self-contradictory
  combination some hosts reject).
- VBUSEN routed as GPIO output LOW (peripheral routing makes the
  USB module drive VBUSEN HIGH = host mode, blocking device
  enumeration).
- `g_tz_nsc_cgc_usb_init_step` boot-step tracker added so the
  next iteration can localize which veneer call is failing.

## How to graduate back

1. Resolve the NSC veneer wiring: confirm
   `libs/ra_nsc/src/ra_nsc_cgc.c` is linked into the Secure
   image, and that `trustzone_init.c` programs an SAU region
   marking the veneer page as NSC (RLAR.NSC = 1).
2. Re-flash; if `g_tz_nsc_cgc_usb_init_step` advances past 7 and
   `g_tz_nsc_cgc_usb_match` starts climbing, the bridge works.
3. Promote back to `hw_validated/hil/` once enumeration is
   steady (VID 1209:000a / 32/32 round-trip on
   `usb_benchmark.py --quick`).
