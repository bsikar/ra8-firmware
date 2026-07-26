# eth_tsn_tas_demo

Programs the RA8D2 Ethernet Agent (ETHA) time-sensitive-networking shapers
that no other example referenced (recon gap #134), logging over SCI8 (PD_02 /
PD_03 -> J-Link OB CDC). LED1 blinks as a heartbeat.

## Gap note: `ra8_tsn` is the temperature sensor, not TSN networking

Recon #134 named `ra8_tsn` as the "time-sensitive networking" driver, but
`libs/ra8_hal/ra8_tsn` is actually the on-die **temperature sensor** (already
demonstrated by `adc_diag_tsn_demo`, #183). The real TSN networking surface on
this part is the **ETHA shaper block**, and that is what this example drives:

- `ra8_etha_set_tas_schedule` / `ra8_etha_enable_tas` -- the time-aware shaper
  (TAS / 802.1Qbv scheduled traffic), programmed with a 2-entry gate-control
  list (window 0 opens the class-7 PTP/control gate; window 1 opens the
  best-effort classes 0-6).
- `ra8_etha_configure_cbs` / `ra8_etha_get_cbs_state` -- the credit-based shaper
  (CBS / 802.1Qav) on traffic class 2 (AVB class A).
- `ra8_etha_get_status` -- the TAS cycle-time monitor.

TAS uses the gPTP time base as its schedule reference, so the app brings up
`ra8_eth_gptp` first, then `ra8_etha_init` on port 0 in CONFIG mode (the only
mode in which the shaper registers are writable).

Console output per cycle:

    tsn: tas_entries=2
    tsn: cbs_en=1 gate=1
    tsn: tas_cycle=0
    tsn: schedule PASS

The verdict `tsn: schedule PASS` prints only when every shaper call returned
`k_ra8_ok`. The shaper values (window duration, cycle time, CBS increment /
upper limit) are illustrative round numbers -- the point is exercising the
driver's programming path, not a specific traffic profile.

HUM references: Ch 32 "Ethernet Agent (ETHA)" -- EATASC / EATASGL (TAS),
EACAEC / EACAIVC / EACAULC (CBS). The example adds no raw MMIO of its own;
every register access lives behind the driver API, which carries the citations.

## Tier: hw_pending (bench-only)

`tools/ra8_emulator` has no Ethernet / ETHA peripheral model, so there is no
`make sim-` gate for this app; the EK-RA8D2 Ethernet wire is also marginal
(#21). The example's value is demonstrating the real shaper API behind a clean
ARM cross-build, matching the driver-gap example wave (#182-188).

Asserting the egress is actually shaped to the schedule needs a multi-node
measurement rig plus the gPTP time base locked to a peer (bench wiring #89);
promote to `hw_validated/hil/` once the schedule is confirmed on silicon.

Build:

```
make eth_tsn_tas_demo
```
