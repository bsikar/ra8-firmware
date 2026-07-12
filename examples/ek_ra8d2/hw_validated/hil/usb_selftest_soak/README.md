# usb_selftest_soak (USB self-loop endurance + benchmark)

The endurance/benchmark member of the self-loop matrix. Same dual-stack image as
config A (HS host + FS device on the J7<->J11 cable, no PC in the loop), but the
host worker **repeats** the 1 MiB MRAM integrity sweep `k_selftest_soak_iters`
times back to back instead of once.

- **USBHS (J7) = host:** first-party polled host MSC (`ra8_usb_hmsc` + `ra8_fs`).
- **USBFS (J11) = device:** USBX MSC class, 1 MiB MRAM as a read-only FAT16
  volume (`MRAM.BIN`).

Every 4 KiB `READ(10)` burst is still memcmp'd against the real MRAM window, so a
**single** corrupted or dropped transfer anywhere in the soak fails the run. The
volume and elapsed time are summed across all iterations for a stable throughput
benchmark, then the read-only `WRITE(10)` rejection is confirmed once at the end
(the write STALLs the bulk-OUT pipe, so it runs last).

## Why this matters

`hs_host` proves one clean pass; this proves the transport stays **byte-perfect
under sustained load** and yields a representative throughput number -- the soak
+ benchmark half of the robustness sweep (#92), entirely on-chip.

## Result (validated 2026-06-15 on real hardware)

Console (SCI8 / J-Link OB CDC, 115200):

```
host up on USB-HS, probing the loop...
enumerated vid=0x1209 pid=0x000E over the loop cable
mounted fs=fat16
write rejected (code 0x00000204), MRAM protected
soak: 16 iters, 16 MiB verified in 98637 ms (166 KiB/s), 0 errors
USB SELFTEST SOAK PASS
```

16 MiB streamed over USB == MRAM `0x02000000` byte-for-byte across every
iteration, 0 errors. `s_dbg_pass_count` mirrors the completed-iteration count for
a live J-Link readout of soak progress.

## Tunables

`k_selftest_soak_iters` (default 16) sets the soak length; each iteration is one
full 1 MiB sweep (~6 s on the HS host). Bump it for a longer endurance run.

## Pinout / VID-PID

Identical to `usb_selftest_hs_host` (config A): device VID 0x1209 PID 0x000E;
FS device on J11, HS host on J7, console on SCI8 (PD_02/PD_03). Bench use only.
