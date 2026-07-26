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
bring-up is shimmed to no-ops there -- so there is no `make sim-` gate for this
app. The EK-RA8D2 Ethernet wire is also marginal (#21). The example's value is
demonstrating the real driver API behind a clean ARM cross-build, matching the
driver-gap example wave (#182-188).

A full sync-offset assertion needs a gPTP peer on the wire (bench wiring #89);
promote to `hw_validated/hil/` once the clock is confirmed advancing on silicon.

Build:

```
make eth_gptp_timestamp_demo
```
