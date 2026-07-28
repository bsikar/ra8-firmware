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

TAS uses the gPTP time base as its schedule reference, so the app starts that
counter for real first -- `ra8_cgc_eswclk_init`, then `ra8_eth_gptp_init`
(which derives `PTPTIVCt` from the live ESWCLK) and `ra8_eth_gptp_timer_enable`
-- and then runs `ra8_etha_init` on port 0 in CONFIG mode (the only mode in
which the shaper registers are writable). Before #498 the time base was never
started at all: the app programmed a gate-control list against a counter fixed
at zero.

Console output per cycle:

    tsn: gptp_adv_ns=200000640 sys_ms=200
    tsn: tas_entries=2
    tsn: cbs_en=1 gate=1
    tsn: tas_cycle=0
    tsn: schedule PASS

## What the verdict proves, and what it does not

`tsn: schedule PASS` requires two things:

1. **A real hardware assertion.** The 78-bit gPTP counter is sampled either
   side of a 200 ms SysTick-timed window and must have advanced by that
   interval to within 10 %. If the time base is not running, the app fails.
2. **That every shaper call returned `k_ra8_ok`.** This half proves only that
   the arguments were accepted and the register writes were issued. ETHA stays
   in CONFIG mode here, so **no frame is ever transmitted and nothing about
   shaped egress is measured.**

The shaper values (window duration, cycle time, CBS increment / upper limit)
are illustrative round numbers -- the point is exercising the driver's
programming path, not a specific traffic profile.

HUM references: Ch 32 "Ethernet Agent (ETHA)" -- EATASC / EATASGL (TAS),
EACAEC / EACAIVC / EACAULC (CBS). The example adds no raw MMIO of its own;
every register access lives behind the driver API, which carries the citations.

## Tier: hw_pending (bench-only)

`tools/ra8_emulator` has no ETHA shaper or GPTP timer model -- both windows
fall to the sparse config-reflect fallback, where a counter cannot advance --
so this app would (correctly) report `schedule FAIL` under emulation and there
is no `make emu-` gate for it. The EK-RA8D2 Ethernet wire is also marginal
(#21).

### Why a measurement peer still does not unblock this (#292)

A measurement peer is provisioned on the bench for this app -- `linuxptp` on
the HIL Pi's built-in Ethernet port, which carries a real PTP hardware clock.
#292 recorded two firmware blockers standing between it and a shaped-egress
assertion. One is now closed and one is not:

- **CLOSED: the schedule reference is now started.** TAS times its gate-control
  list against the gPTP timer. `ra8_eth_gptp_init` used to write an invented
  `GPTP_CTRL`/`GPTP_STS`/`GPTP_IE` window instead of the real HUM Ch 35
  `PTPTMEC` / `PTPTIVCt` registers, so the timer was never enabled; #498
  rewrote the driver onto the real map and this app now enables the timer and
  asserts that it advances.
- **OPEN: nothing is transmitted.** `ra8_etha_init` leaves port 0 in `CONFIG`
  mode (the only mode in which the TAS/CBS registers are writable) and the app
  never moves it to `OPERATION`, never opens a queue, and never queues a frame.
  A shaper with no egress produces nothing to measure.

So `tsn: schedule PASS` says the time base runs and the shaper *programming*
calls returned `k_ra8_ok`; it still says nothing about shaped traffic.
Promotion needs this app extended to actually transmit -- not a bench change.

Build:

```
make eth_tsn_tas_demo
```
