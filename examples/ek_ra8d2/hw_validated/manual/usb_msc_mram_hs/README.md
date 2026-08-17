# usb_msc_mram_hs

`usb_msc_mram` on the USB-HS port (J7): the same synthesized read-only FAT16 view
of the chip's on-board MRAM, at high speed. What differs is the HS device
framework -- Device Qualifier descriptor, 512-byte bulk max packet size, a
full-speed fallback framework -- and the USBHS controller plus UTMI PHY bring-up.

Verification is the same by-hand comparison: copy `MRAM.BIN` off over USB, dump
the same window over SWD with `savebin`, and require the bytes to be identical.

## Four device-path defects that HS mass storage flushed out (#67)

The HS CDC class worked from day one. Mass storage at high speed exercised paths
CDC never touched:

1. **INTSTS1 latch storm.** The USBHS controller latches BCHG / DTCH / ATTCH in
   INTSTS1 even with INTENB1 clear, holding the NVIC line asserted. The
   resulting spurious-ISR storm starved every thread -- the storage thread never
   got a timeslice. Fixed by porting the FS storm guard (mask the line after a
   run of spurious entries, with a SysTick hook re-enabling it) plus a W0C
   acknowledgement of those bits.
2. **DVSQ mirror policy.** USBHS INTSTS0 bit 7 is VBUS status, not suspend --
   suspend is bit 6 on both controllers -- and the hardware DVSQ lags the USBX
   stack during SET_CONFIGURATION. State mirroring is therefore upgrade-only,
   and the stack is told about a disconnect only on a true Default-state entry
   from a real bus reset.
3. **Forbidden ZLP in BOT.** Auto-staging a zero-length packet after any
   max-packet-exact IN puts a ZLP where the host expects the CSW, wedging the
   transport. The bridge now honours the stack's explicit ZLP flag exactly,
   staged from the BEMP completion path and never at submit time.
4. **Double-banking is unreliable on device bulk-IN.** Staging the next packet
   after a max-packet-exact fill FRDY-times-out on a double-banked pipe. Device
   mode therefore runs bulk IN single-banked; host mode keeps double-banking,
   because its validated ladders depend on it.

## Diagnostics

The app and the bridge keep JLink-readable state: device-state, speed, class
instance and framework mirrors, storage-thread and media-read counters, a
BOT/SETUP event trace ring with DWT cycle timestamps, and a DVSQ causal history
whose high nibble is the raw DVSQ and low nibble the device state at IRQ entry.
They are statics, so re-resolve their addresses with `arm-none-eabi-nm` after
every build.

## Board facts

The USBHS VBUS sense pin is the only PFS-muxed HS pin; D+ and D- are dedicated
PHY balls. J7's role-select GPIO is driven LOW for device mode (EK-RA8D2 v1 UM
Sec 6.2 p 34). The USB IDs come from the pid.codes free-for-experiments range
and are bench-only.
