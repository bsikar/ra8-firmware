# USB CDC / NSC USB apps (hw_pending)

usb_cdc_echo and tz_nsc_cgc_usb both rely on USB-FS bring-up:
CGC PLL2 + USBCKCR + USBFS module + USBX class driver.

## Why these are `hw_pending`

JTAG probing of the live HIL board on 2026-05-19 confirmed:

- OSCSF = 0x00 -- no oscillators stable when the probe halted
  (HOCO / Main XTAL / PLL1 / PLL2 all flagged not-stable).
- PLL2CR.PLL2STP = 1 (PLL2 stopped).
- PLL2CCR  = 0 (never programmed).
- USBCKCR  = 0, USBCKDIVCR = 0 (never programmed).
- PC parked in ra8_exception_halt_loop, LR in ra8_exception_report.
- g_ra8_exception_last is all zero (the report path apparently
  silently aborted before writing the snapshot).
- No UART output -- the SCI8 console never came up either.

The firmware crashes very early -- before `ra8_cgc_init` finishes
or even reaches `ra8_log_error_val`. Without further per-step
instrumentation it's not possible to tell whether the crash is in
SystemInit, in ra8_cgc_init's PLL1 bring-up, or in the USBX memory
pool / class driver init.

## How to graduate back

Same step-tracker pattern used in flash_journal / cpu1_pingpong:

1. Add `g_usb_step` volatile counter at module scope.
2. Stamp it 1 right after entering `main`, 2 after `ra8_cgc_init`,
   3 after `ra8_pfs_route_peripheral` for USB pins, 4 after
   USBX init, 5 in the first loop iteration.
3. Memprobe `g_usb_step` to find the failing step, then JTAG-dump
   the relevant peripheral state.

`tz_secure_only_usb_fs` and `tz_secure_only_usb_hs` continue to pass
on the simpler non-ThreadX path; the issue is specific to the
USBX-based stack.
