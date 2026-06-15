# usb_msc_mram_hs

Plug the EK-RA8D2's **USB-HS port (J7)** into any computer and the
chip's **onboard 1 MiB MRAM shows up as a file -- at 480 Mbps**.

USB-HS device-mode twin of `usb_msc_mram` (same synthesized read-only
FAT16 volume: boot sector / FAT / root directory generated on the fly,
data clusters mapped 1:1 onto the MRAM code window at `0x02000000`,
one root file `MRAM.BIN`, write-protected via MODE SENSE, WRITE(10)
rejected with DATA PROTECT sense). The differences are the HS device
framework (Device Qualifier descriptor, 512-byte bulk MPS, FS-fallback
framework) and the USBHS controller + UTMI PHY bring-up
(`ra_cgc_usbhs_pll_enable`, HSE/CNEN, dedicated PHY balls).

## Build

```
make build
make flash
```

## Verify (macOS) -- validated 2026-06-12 on real hardware

Enumerates at 480 Mbps (`UsbLinkSpeed = 480000000` in ioreg) and
auto-mounts read-only as `/Volumes/RA8D2 MRAM`:

```
ls -l "/Volumes/RA8D2 MRAM/"            # MRAM.BIN, 1048576 bytes
# Reference dump of the same window over SWD:
#   JLinkExe> savebin /tmp/mram_ref.bin 0x02000000 0x100000
cmp "/Volumes/RA8D2 MRAM/MRAM.BIN" /tmp/mram_ref.bin && echo IDENTICAL
```

Evidence from the validation run (fresh attach, page cache cold):

- 1,048,576 bytes copied in 0.53 s = **1.97 MB/s real wire rate**
  (4.5x the FS twin on the same host).
- `cmp` vs the SWD `savebin` dump: **IDENTICAL**; SHA-256 of both
  starts `1882fd0e3c1d84d8f75baab8425601`.
- Three consecutive re-copies hash identical.
- HS CDC regression (`tz_secure_only_usb_hs` echo test) still passes
  on the same DCD.

## Verify (Linux)

```
lsusb                                    # "EK-RA8D2 MRAM HS" (1209:000d)
udisksctl mount -b /dev/sdX1 2>/dev/null || sudo mount -o ro /dev/sdX /mnt
md5sum /mnt/MRAM.BIN                     # compare vs a JLink savebin dump
```

## What HS device-mode MSC took (DCD fixes, see #67)

The HS CDC class worked from day one; MSC at HS flushed out four real
defects in the USBHS device path that FS / CDC never exercised:

1. **USBHS INTSTS1 latch storm.** The controller latches BCHG / DTCH /
   ATTCH in INTSTS1 even with INTENB1 = 0, holding the NVIC line: a
   440 kHz spurious-ISR storm starved every thread (the storage thread
   never got one timeslice). Fixed by porting the FS storm guard
   (mask the line after a run of spurious entries; 1 ms SysTick hook
   re-enables) plus a W0C ack of those INTSTS1 bits.
2. **DVSQ mirror policy.** USBHS INTSTS0 bit 7 is VBUS status (not
   suspend -- suspend is bit 6 on both controllers); and hardware DVSQ
   lags the USBX stack during SET_CONFIGURATION, so state mirroring is
   upgrade-only, with the stack disconnect issued only on a true
   Default-state entry (real bus reset).
3. **Forbidden ZLP in BOT.** The DCD auto-staged a ZLP after any
   MPS-exact IN; a 512-byte data phase at 512 MPS got a ZLP where the
   host expected the CSW, wedging the transport. The DCD now honors
   the stack's `force_zlp` flag exactly: ZLP staged from the BEMP
   completion path, never at submit time.
4. **DBLB double-banking unreliable on device bulk-IN.** Staging the
   next packet after an MPS-exact fill FRDY-times-out on a
   double-banked pipe. Device-mode bulk IN now runs single-banked;
   host mode keeps DBLB (its validated ladders depend on it).

## Diagnostics built into this app

JLink-readable probes (re-resolve addresses with `arm-none-eabi-nm`
after every build): `s_dbg_dev_state`, `s_dbg_ux_speed`,
`s_dbg_class_inst`, `s_dbg_framework` / `s_dbg_fw_len`,
`s_dbg_thr_state` / `s_dbg_thr_runs` (storage thread TCB),
`s_dbg_activates` / `s_dbg_deactivates` (class callbacks),
`s_dbg_state3_seen` (CONFIGURED sampler), `s_dbg_err_*` (USBX error
callback), `s_dbg_read_*` (media reads). The DCD adds a BOT/SETUP
event trace ring (`s_trace` / `s_trace_seq` with DWT cycle timestamps
in `s_trace_ts`) and a DVST causal history (`s_dvst_state_history`:
high nibble = raw DVSQ, low nibble = device state at IRQ entry).

## Pinout

P4_08 = USBHS_VBUS sense (PSEL 0x14; the only PFS-muxed HS pin).
PD07 = J7 role select, driven LOW for Device mode (EK-RA8D2 v1 UM
Sec 6.2 p 34). D+/D- are dedicated HS PHY balls.

## VID / PID

VID = 0x1209 (pid.codes free-for-experiments range), PID = 0x000D,
serial 00000003. Bench use only.
