# usb_msc_mram_hs (hw_pending -- bring-up in progress)

USB-HS device-mode twin of `hw_validated/hil/usb_msc_mram`: the board
should enumerate on the J7 USB-HS port at 480 Mbps as a read-only
FAT16 volume exposing the 1 MiB MRAM window as `MRAM.BIN`. The FS
variant is fully validated against macOS; this HS variant is NOT yet
working and stays in `hw_pending/`.

## Bring-up status (2026-06-12)

Working at HS on a real macOS host:

- 480 Mbps link (UsbLinkSpeed = 480000000), EP0, descriptors (HS
  framework with Device Qualifier + 512-byte bulk MPS), strings,
  GET_MAX_LUN. The validated HS CDC app (`tz_secure_only_usb_hs`)
  binds and serves `/dev/cu.usbmodem*` with the same DCD, including
  after the DCD fixes below -- the HS device path itself is healthy.

Blocked:

- macOS configures the device, then resets/re-enumerates in a tight
  loop; the configured windows are sub-millisecond (a 1 kHz on-device
  sampler never observes `UX_DEVICE_CONFIGURED`), so the USBX storage
  thread (resumed at class activation, gated on CONFIGURED) never gets
  a single timeslice (`tx_thread_run_count == 0` across all cycles)
  and the bulk-OUT pipe is never armed. Class activate/deactivate
  cycle correctly with the disconnect-on-bus-reset fix (counters
  reached 6/5), so the chapter-9 plumbing churns as designed -- the
  open question is why the host kills each HS MSC configuration
  within a millisecond when the same host accepts the HS CDC
  configuration and the FS MSC configuration outright.

Next steps: timestamp the configure/reset events (cycle-counter delta
between CHANGE_STATE(CONFIGURED) and the next bus reset) to measure
the true window; capture whether SET_CONFIGURATION's EP0 status stage
completes at HS for this 32-byte MSC config (the CDC config is 75
bytes and works); compare against a Linux host on the HS port.

## Diagnostics built into this app

JLink-readable probes (see `arm-none-eabi-nm` for addresses):
`s_dbg_dev_state`, `s_dbg_slave_speed`, `s_dbg_class_inst`,
`s_dbg_framework` / `s_dbg_fw_len`, `s_dbg_thr_state` /
`s_dbg_thr_runs` (storage thread TCB), `s_dbg_activates` /
`s_dbg_deactivates` (class callbacks), `s_dbg_state3_seen` (1 kHz
CONFIGURED sampler), `s_dbg_err_*` (USBX error callback),
`s_dbg_read_*` (media reads). The DCD adds a BOT/SETUP event trace
ring (`s_trace`/`s_trace_seq`) and a DVST causal history
(`s_dvst_state_history`: high nibble = raw DVSQ, low nibble =
`ux_slave_device_state` at IRQ entry).

## Pinout

P4_08 = USBHS_VBUS sense (PSEL 0x14; the only PFS-muxed HS pin).
PD07 = J7 role select, driven LOW for Device mode (EK-RA8D2 v1 UM
Sec 6.2 p 34). D+/D- are dedicated HS PHY balls.

VID/PID = 0x1209:0x000D, serial 00000003. Bench use only.
