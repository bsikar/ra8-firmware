# usb_selftest_dfu (a DFU download/upload loop, verified on-chip)

The **control-transfer** counterpart to the CDC / MSC self-loops. It drives a
full host DFU_DNLOAD -> device capture -> host DFU_UPLOAD -> byte-check
round-trip through a real **DFU device class**, and is the on-bench validation
that the device EP0 **control-OUT data stage** (host->device SETUP data) works.
The two USB ports are cabled **to each other** and one firmware image runs both
USB stacks.

- **USBFS (J11) = device:** a ThreadX + USBX DFU class started directly in
  `dfuIDLE` (DFU-mode interface, class 0xFE / subclass 0x01 / protocol 0x02,
  DFU functional descriptor wTransferSize 64). Its `write` callback captures
  each DFU_DNLOAD block into a 512-byte image buffer; its `read` callback
  serves the same bytes back on DFU_UPLOAD.
- **USBHS (J7) = host:** a self-contained polled DFU host built on the
  first-party `ra8_usb_host_*` primitives. It enumerates the device (bus reset
  -> GET_DESCRIPTOR -> SET_ADDRESS -> SET_CONFIGURATION), then runs the
  round-trip: 8 x DFU_DNLOAD of a deterministic 64-byte pattern (each followed
  by DFU_GETSTATUS to `dfuDNLOAD-IDLE`), DFU_ABORT back to `dfuIDLE`, then
  8 x DFU_UPLOAD read-back with a byte-exact compare.

Each block is one full 64-byte control data stage (one DCP bank, MPS-exact).
The download closes with **DFU_ABORT** rather than the spec's zero-length
DFU_DNLOAD manifest: this USBX DFU class is not manifestation-tolerant, so
after MANIFEST it parks in `dfuMANIFEST-WAIT-RESET` and only a USB bus reset
returns it to a usable state -- which would tear down the in-place UPLOAD
round-trip. DFU_ABORT returns `dfuDNLOAD-IDLE` -> `dfuIDLE` without a reset.

## The driver fix this leans on

The device side rides a new **EP0 control-OUT data receive** in the
`ux_dcd_ra8_usb` bridge. A control-WRITE data stage cannot be received
synchronously in the SETUP ISR: the FS device ISR and the lower-priority HS
host worker share one CPU, so a blocking receive would spin out the very thread
that must SEND the data (a same-CPU deadlock). Instead the SETUP ISR *arms* the
DCP (`ra8_usb_dcp_out_arm`) and returns; the subsequent DCP BRDY IRQ drains the
bank (`ra8_usb_dcp_out_read`) and only then runs the chapter-9 dispatcher.
On the host the DCP must set `DCPCFG.DIR = 1` for the OUT data stage (the
default issues IN tokens), and the device drives `DCPCTR.SQSET` (DATA1) +
clears a stale `CCPL` before arming so the SIE accepts the data instead of
flagging `CTSQ = SQER`.

## Result (validated 2026-06-16 on real hardware)

Fresh-reset console (SCI8 / J-Link OB CDC, 115200):

```
ra8d2 dfu: host up on USB-HS, probing the loop...
ra8d2 dfu: enumerated pid=0x0019
ra8d2 dfu: 8 blocks downloaded + verified -- USB SELFTEST DFU PASS
```

J-Link probes confirm the round-trip: `s_dbg_pass_count == 1`,
`s_dbg_blocks_ok == 8`, `s_dbg_pid == 0x0019`,
`s_dbg_mismatch == 0xFFFFFFFF` (every uploaded block read back equal),
`s_dfu_image_len == 0x200` (full 512-byte image captured),
`s_dbg_dev_writes` advances per DNLOAD block. Deterministic across fresh
resets. The committed CDC / MSC self-loops stay green with the same bridge.

## Diagnostics (J-Link, re-resolve with `arm-none-eabi-nm`)

`s_dbg_phase` (1 init, 2 enum, 3 download, 4 upload, 5 pass), `s_dbg_blocks_ok`,
`s_dbg_pid`, `s_dbg_mismatch` (first differing block), `s_dbg_pass_count`,
`s_dbg_dev_writes` (device DNLOAD write-callback count),
`s_dfu_image_len` (bytes captured into the device image buffer).

## Pinout

FS device: P4_07 VBUS sense, P5_00 VBUSEN GPIO LOW, P8_14/P8_15 data
(PSEL usb_fs). HS host: SW4-8 Host via the U15 expander, PD07 HIGH (U18
powers J7), P4_08 VBUS sense (PSEL usb_hs). Console: PD_02/PD_03 SCI8.

## VID / PID

Device side advertises VID 0x1209, PID 0x0019. Bench use only.
