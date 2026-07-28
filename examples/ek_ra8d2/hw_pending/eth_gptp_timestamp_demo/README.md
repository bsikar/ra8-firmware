# eth_gptp_timestamp_demo

Drives the Ethernet gPTP / IEEE 1588 hardware-timestamp path that no other
example referenced (recon gap #133), logging results over SCI8 (PD_02 / PD_03
-> J-Link OB CDC). LED1 blinks as a heartbeat.

Two layered drivers are exercised end to end:

- `ra8_eth_gptp` -- brings up the GPTP block that owns the free-running IEEE
  1588 hardware timestamp counter shared by the GMAC ports, and reads its
  status word (`ra8_eth_gptp_init`, `ra8_eth_gptp_get_status`).
- `ra8_ptp` -- the SYNFP / STCA IEEE 1588-2019 clock layered on the counter:
  - `ra8_ptp_open` / `ra8_ptp_set_role` (role `k_ra8_ptp_role_controller`),
  - `ra8_ptp_set_time` + `ra8_ptp_get_time` -- the timestamp capture path,
  - `ra8_ptp_adjust_time` + `ra8_ptp_adjust_rate` -- the disciplined-clock servo
    (step + rate), `ra8_ptp_get_offset`,
  - `ra8_ptp_send_sync` + `ra8_ptp_send_announce` -- the transmit-side generators.

Console output per cycle:

    gptp: t=1000000000.0
    gptp: t=1000000000.0
    gptp: offset_ns=0
    gptp: sts=0x0
    gptp: clock PASS

The two `gptp: t=` lines are back-to-back captures of the hardware counter, so
on silicon the nanoseconds field advances between them. The verdict
`gptp: clock PASS` prints only when every driver call in the cycle returned
`k_ra8_ok`.

HUM references: Ch 31-32 "Ethernet" (GPTP timer + gPTP). The example adds no
raw MMIO of its own -- every register access lives behind the driver API,
which already carries its HUM citations.

## Tier: hw_pending (bench-only)

`tools/ra8_emulator` has no Ethernet / GPTP peripheral model -- the GWCA/ETHA/PHY
bring-up is shimmed to no-ops there -- so there is no `make emu-` gate for this
app. The EK-RA8D2 Ethernet wire is also marginal (#21). The example's value is
demonstrating the real driver API behind a clean ARM cross-build, matching the
driver-gap example wave (#182-188).

### Why a bounded sync offset is NOT the missing piece (#292)

A gPTP peer was attached to the bench for this app -- the EK-RA8D2's RJ45 now
goes to the HIL Pi's built-in Ethernet port, which has a real PTP hardware
clock (`/dev/ptp0`, hardware transmit + receive timestamping), and `linuxptp`
is provisioned there by the `hil_bench` Ansible role. **That did not unblock
this app, and no peer ever will as it stands.** Two facts, in order:

1. **The RA8D2 has no PTP message engine.** HUM Ch 35 "Ethernet Generic PTP
   Timer (GPTP)" (p 1925-1964) is a *timer*: enable/disable (`PTPTMEC` /
   `PTPTMDC`), an increment value in ns per clk (`PTPTIVCt`), a 78-bit offset
   load (`PTPTOVCtL/M/U`), 78-bit monitoring (`PTPGPTPTMtL/M/U`), plus PPS,
   media-clock capture/recovery and cyclic compare. There is no Sync/Announce
   generator, no domain register, no clockIdentity, no port role, no BMCA. The
   only other PTP hardware on the part is RMAC timestamp *capture* with PTP
   frame filtering (HUM Ch 33, `MTRC` / `MPFCt`). Everything above that --
   Sync, Follow_Up, Pdelay, the servo, the state machine -- is software on this
   silicon, and no such stack drives the RA8D2 Ethernet path in this tree.
2. **The `ra8_ptp` driver this app calls does not address real registers.**
   `libs/ra8_hal/inc/ra8_ptp_regs.h` declares a "SYNFP/STCA window" at
   `0x403E_0100` with registers (`PTP_CTRL`, `PTP_DOMAIN`, `PTP_SYNINT`,
   `PTP_TIME_SECH`, `PTP_TX_TRIG`, `PTP_MAC0`, ...) that appear nowhere in the
   HUM; `0x403E_0100` is a reserved hole between `PTPGPTPTM1U` (`+0x0098`) and
   the media-clock block (`+0x0200`). The `r_gptp_regs_t` window in
   `ra8_ether_regs.h` is invented the same way -- its `GPTP_CTRL` at `+0x00` is
   really the read-only `PTPIPV` IP-version register. So `gptp: clock PASS`
   asserts only that a reserved aperture read back what was written to it, and
   `ra8_ptp_send_sync()` puts nothing on any wire.

The blocker is therefore the driver, not the bench. Rewriting `ra8_eth_gptp`
onto the real Ch-35 register map is tracked separately; until then this app
must stay in `hw_pending`, and its `hil.conf` verdict must not be read as
evidence that the hardware clock works.

Build:

```
make eth_gptp_timestamp_demo
```
