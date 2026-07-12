# usb_selftest_cdc (a CDC-ACM echo loop, verified on-chip)

The **bidirectional bulk** counterpart to the MSC self-loops. It drives a
full host bulk-OUT -> device receive -> device echo -> host bulk-IN
round-trip through a real **CDC-ACM device class**, and is the on-bench
validation that the device bulk-OUT WRITE(10) driver fix carries over to
a non-MSC class. The two USB ports are cabled **to each other** and one
firmware image runs both USB stacks.

- **USBFS (J11) = device:** a ThreadX + USBX CDC-ACM class. A worker loops
  `_ux_device_class_cdc_acm_read` -> `_ux_device_class_cdc_acm_write`,
  echoing every byte the host sends back on the bulk-IN endpoint. This is
  the **worker-thread** echo (read then write), *not* the DCD ISR
  auto-echo -- the worker path rides the normal device bulk-OUT receive
  that the WRITE(10) fix repaired, so it does not storm in the dual-stack
  loop the way auto-echo did.
- **USBHS (J7) = host:** a self-contained polled CDC host built on the
  first-party `ra8_usb_host_*` primitives (no `ra8_usb_hcdc`). It enumerates
  the device (bus reset -> GET_DESCRIPTOR -> SET_ADDRESS ->
  SET_CONFIGURATION), opens the CDC data interface's bulk pipes (EP2 OUT /
  EP1 IN, 64-byte MPS), then runs 8 rounds: bulk-OUT a deterministic
  60-byte pattern, bulk-IN the echo, and byte-check it.

Each round ships a **sub-MPS (60-byte)** payload so the echo returns as a
single short packet -- no MPS-exact ZLP ambiguity on the host bulk-IN.
No serial terminal is involved; raw bulk transfers only.

## The driver fix this leans on

The device side uses the worker-thread CDC read/write path, which rides
the same device bulk-OUT receive that the writable self-loops validated
(host MPS chunking + device DBLB + DCD loop-drain). The earlier CDC
attempt used the DCD ISR auto-echo and stormed in the dual-stack loop;
the worker-thread path avoids that entirely.

## Result (validated 2026-06-13 on real hardware)

Fresh-reset console (SCI8 / J-Link OB CDC, 115200):

```
ra8d2 cdc: host up on USB-HS, probing the loop...
ra8d2 cdc: enumerated pid=0x0017
ra8d2 cdc: 8 rounds echoed -- USB SELFTEST CDC-ECHO PASS
```

J-Link probes confirm the round-trip: `s_dbg_pass_count == 1`,
`s_dbg_rounds_ok == 8`, `s_dbg_pid == 0x0017`,
`s_dbg_mismatch == 0xFFFFFFFF` (every echoed round read back equal),
`s_dbg_dev_step == 5` (device echo loop live), `s_dbg_dev_err == 0`,
`s_dbg_dev_echo_calls == 8`, `s_dbg_dev_last_len == 0x3C` (60 bytes).
Deterministic across fresh resets. The committed MSC self-loops stay
green with the same driver fix.

## Diagnostics (J-Link, re-resolve with `arm-none-eabi-nm`)

`s_dbg_phase` (1 init, 2 enum, 3 echo rounds, 4 pass), `s_dbg_rounds_ok`,
`s_dbg_pid`, `s_dbg_mismatch` (first differing round), `s_dbg_pass_count`,
`s_dbg_dev_step` / `s_dbg_dev_err` (device-worker progress),
`s_dbg_dev_echo_calls` / `s_dbg_dev_last_len` (device echo activity).

## Pinout

FS device: P4_07 VBUS sense, P5_00 VBUSEN GPIO LOW, P8_14/P8_15 data
(PSEL usb_fs). HS host: SW4-8 Host via the U15 expander, PD07 HIGH (U18
powers J7), P4_08 VBUS sense (PSEL usb_hs). Console: PD_02/PD_03 SCI8.

## VID / PID

Device side advertises VID 0x1209, PID 0x0017. Bench use only.
