# eth_gptp_timestamp_demo

Brings up the RA8D2 Ethernet Generic PTP Timer (HUM Ch 35, p 1925-1964) and then
*measures* it.

## What this part actually has

HUM Ch 35 describes a **timer**, not a protocol engine. Its whole register list
(Table 35.3, p 1926) is: an IP-version word, timer enable / disable, a per-clk
increment, a 78-bit offset, 78-bit and 64-bit time monitors, media-clock capture
and recovery, cyclic compare, and a pulse-output timer. There is **no** Sync or
Announce generator, no `Follow_Up`, no domain register, no `clockIdentity` and no
BMCA anywhere in the block. An IEEE 1588-2019 / IEEE 802.1AS implementation on
this silicon is therefore software above the HAL, disciplining this counter and
taking receive timestamps from the RMAC capture path (`MTRC` / `MPFCt`, HUM
Ch 33).

An earlier version of this app drove an invented `ra8_ptp` "SYNFP/STCA" register
window at `0x403E_0100` -- a reserved hole in the GPTP aperture -- and printed
`PASS` because a reserved region echoed back its own writes. Issue #498 records
that; the fiction is deleted.

## What the app does

Bring-up runs CGC, then `ra8_cgc_eswclk_init` (the ESWM power domain and ESWCLK,
which is the GPTP `clk` input), then the module-stop release. `ra8_eth_gptp_init`
derives `PTPTIVCt` from the live ESWCLK frequency, a known epoch is loaded into
the 78-bit offset, and timer unit 0 starts. Each cycle then makes two
assertions:

- **Presence.** The read-only `PTPIPV` word must be nonzero. That is the probe
  that this is a real aperture: a reserved region reads back zero or whatever was
  last written, whereas `PTPIPV` has a nonzero reset value (HUM 35.3.1.1 p 1927).
- **Advance.** The 78-bit counter is sampled either side of a SysTick-timed
  window and must have advanced by that interval to within 10 %.

So the verdict means the counter advances one second per second against an
independent time base. **It is not a claim that the part speaks gPTP.** The
example adds no raw MMIO of its own; every register access lives behind the
driver API, which carries its own HUM citations.

## Blocked on

`ra8_emulator` has no GPTP timer model -- the window falls to the sparse
config-reflect fallback, where a counter cannot advance -- so the app would
correctly report failure under emulation. The EK-RA8D2 Ethernet wire is also
marginal (#21).

A gPTP peer *is* attached to the bench (the board's RJ45 goes to the HIL Pi's
built-in Ethernet port, which has a real PTP hardware clock and provisioned
`linuxptp`), and it does not unblock this app -- **no peer can**, because the
RA8D2 has no PTP message engine. Sync, Follow_Up, Pdelay, the servo and the
state machine are all software on this silicon, and no such stack drives the
RA8D2 Ethernet path in this tree (#292). Comparing this counter against the
peer's `/dev/ptp0` over a long window remains a manual bench measurement.
