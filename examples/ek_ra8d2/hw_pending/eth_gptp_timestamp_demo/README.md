# eth_gptp_timestamp_demo

Brings up the RA8D2 Ethernet Generic PTP Timer (HUM Ch 35, p 1925-1964) and
then *measures* it, logging over SCI8 (PD_02 / PD_03 -> J-Link OB CDC). LED1
blinks as a heartbeat.

## What this part actually has

HUM Ch 35 describes a **timer**, not a protocol engine. Its whole register list
(Table 35.3, p 1926) is: an IP-version word, timer enable / disable, a per-clk
increment, a 78-bit offset, 78-bit and 64-bit time monitors, media-clock
capture / recovery, cyclic compare, and a pulse-output timer. There is **no**
Sync or Announce generator, no `Follow_Up`, no domain register, no
`clockIdentity` and no BMCA anywhere in the block. An IEEE 1588-2019 /
IEEE 802.1AS implementation on this silicon is therefore software above the
HAL, disciplining this counter and taking receive timestamps from the RMAC
capture path (`MTRC` / `MPFCt`, HUM Ch 33).

An earlier version of this app drove an invented `ra8_ptp` "SYNFP/STCA"
register window at `0x403E_0100` -- a reserved hole in the GPTP aperture -- and
printed `PASS` because a reserved region echoed back its own writes. Issue #498
records that; the fiction is deleted.

## What the app does

Bring-up: CGC -> `ra8_cgc_eswclk_init` (ESWM power domain + ESWCLK, the GPTP
`clk` input) -> MSTP -> SysTick -> console. Then `ra8_eth_gptp_init` derives
`PTPTIVCt` from the live ESWCLK frequency, `ra8_eth_gptp_set_offset` loads a
known epoch into the 78-bit offset, and `ra8_eth_gptp_timer_enable` starts
timer unit 0.

Per cycle:

- `ra8_eth_gptp_ip_version` reads the read-only `PTPIPV` word and requires it
  to be nonzero. That is the presence probe -- a reserved aperture reads back
  zero or whatever was last written, whereas `PTPIPV` has a nonzero reset value
  (HUM 35.3.1.1 p 1927).
- `ra8_eth_gptp_get_time` samples the 78-bit counter, the app waits 500 ms
  measured on SysTick, samples again, and requires the advance to match that
  interval to within 10 %.

Console output per cycle:

    gptp: ipv=3
    gptp: t=1000000000.4187520
    gptp: t=1000000000.919124160
    gptp: adv_ns=500002240 sys_ms=500
    gptp: clock PASS

`gptp: clock PASS` means the counter advanced one second per second against an
independent time base. It is not a claim that the part speaks gPTP.

The example adds no raw MMIO of its own -- every register access lives behind
the driver API, which carries its own HUM citations.

## Tier: hw_pending (bench-only)

`tools/ra8_emulator` has no GPTP timer model: the `0x403E_0000` window falls to
the sparse config-reflect fallback, where a counter cannot advance, so this app
would (correctly) report `clock FAIL` under emulation and there is no `make
emu-` gate for it. The EK-RA8D2 Ethernet wire is also marginal (#21).

### The bench peer exists; a bounded sync offset still is not the assertion (#292)

A gPTP peer is attached to the bench for this app -- the EK-RA8D2's RJ45 goes
to the HIL Pi's built-in Ethernet port, which has a real PTP hardware clock
(`/dev/ptp0`, hardware transmit + receive timestamping), and `linuxptp` is
provisioned there by the `hil_bench` Ansible role.

That peer did not unblock this app, and no peer can, because **the RA8D2 has no
PTP message engine** (see "What this part actually has" above). Sync,
Follow_Up, Pdelay, the servo and the state machine are all software on this
silicon, and no such stack drives the RA8D2 Ethernet path in this tree. So this
app asserts what the hardware genuinely provides -- that the counter runs one
second per second -- and nothing about protocol conformance.

The second half of #292's finding is now closed: the driver addresses the real
Ch-35 registers (#498), so `gptp: clock PASS` is no longer a reserved aperture
reading back what was written to it.

Comparing this counter against the peer's `/dev/ptp0` over a long window is a
manual bench measurement; promote to `hw_validated/hil/` once the advance
assertion is confirmed on silicon.

Build:

```
make eth_gptp_timestamp_demo
```
