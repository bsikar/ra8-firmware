# usb_selftest_hs_host

The board's two USB ports are cabled to each other and ONE image runs BOTH sides
of the link, then verifies itself on-chip -- no PC in the loop. USBFS (J11) is
the `usb_msc_mram` Mass-Storage class exposing the 1 MiB MRAM window at
`0x02000000` as a read-only synthesized FAT16 volume, IRQ-driven through the
`ux_dcd_ra8_usb` device bridge; USBHS (J7) is the first-party polled host MSC
stack (`ra8_usb_hmsc` + `ra8_fs`) in a low-priority ThreadX thread, which mounts
the volume and memcmp's every raw multi-block `READ(10)` burst against the same
MRAM bytes read directly. WRITE(10) into the read-only LUN must be rejected, and
the MRAM is never touched.

The FS device is the speed ceiling, so the HS host serves a full-speed
downstream device (RHST = FS) -- a path the Mac-attached ladders never exercise.
What it proves is that the USBX device stack and the first-party host stack run
concurrently in one image, one controller each, and that the SCSI transport
returns the chip's flash byte for byte with no external host in the loop.

## Bring-up notes (the two non-obvious fixes)

1. **Pre-kernel SysTick.** `main()` starts SysTick, for `ra8_delay_ms`, before
   `tx_kernel_enter`, and this app's setup window is long (the U15 expander I2C
   transaction blocks for milliseconds), so the tick fires before ThreadX timer
   state exists. Feeding `_tx_timer_interrupt` then walks a zeroed expiration
   list and bus-faults. The handler gates ThreadX delivery on `s_tx_kernel_up`,
   set in `tx_application_define`.
2. **Thread priority.** The polled host loop busy-waits for device data, and the
   USBX device storage *class thread* (`UX_THREAD_PRIORITY_CLASS`) is what
   actually runs `media_read`. The host worker must therefore sit BELOW it or
   the class thread can never preempt the spin and every bulk read times out --
   while enumeration still works, since device SETUP is ISR-driven. That was the
   `0x203` mount timeout.

## Known limitation (#92)

The WRITE(10) rejection leaves the bulk-OUT endpoint STALLed. Recovering the BOT
transport afterwards (Bulk-Only Mass Storage Reset + Clear Feature
ENDPOINT_HALT) is not in the host class yet, so the run parks after a single
pass rather than looping.

## Pinout

FS device: P4_07 VBUS sense, P5_00 VBUSEN GPIO LOW (device role), P8_14/P8_15
data (PSEL usb_fs). HS host: SW4-8 Host via the U15 expander, PD07 HIGH (U18
powers J7), P4_08 VBUS sense (PSEL usb_hs). Console: PD_02/PD_03 SCI8. The
device advertises VID 0x1209 with a per-app PID, distinct from the Mac-facing
MRAM apps; bench use only.
