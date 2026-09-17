# eth_tsn_tas_demo

Programs the RA8D2 Ethernet Agent (ETHA) time-sensitive-networking shapers that
no other example referenced (recon gap #134): the time-aware shaper (TAS,
802.1Qbv scheduled traffic) and the credit-based shaper (CBS, 802.1Qav).

## `ra8_tsn` is the temperature sensor, not TSN networking

Recon #134 named `ra8_tsn` as the time-sensitive-networking driver, but
`libs/ra8_hal/inc/ra8_tsn.h` is the on-die **temperature sensor** (demonstrated by
`adc_diag_tsn_demo`). The real TSN networking surface on this part is the ETHA
shaper block, which is what this example drives.

A TAS entry on this part carries a **single gate-state bit** for the queue whose
TAS RAM block holds it (HUM Table 32.6 p 1691) -- not an interleaved per-class
gate vector. The app programs a short gate list on the PTP/control descriptor
queue, one window open and one shut, then reads every entry back out of TAS RAM
and compares.

TAS times its gate-control list against the gPTP counter, so the app starts that
time base for real first and only then runs `ra8_etha_init` in CONFIG mode --
the only mode in which the shaper registers are writable. Before #498 the time
base was never started at all: the app programmed a gate-control list against a
counter fixed at zero.

## What the verdict proves, and what it does not

It requires three things:

1. **A real hardware assertion.** The 78-bit gPTP counter is sampled either side
   of a SysTick-timed window and must have advanced by that interval to within
   10 %. If the time base is not running, the app fails.
2. **A second real hardware assertion.** Every TAS entry programmed is read back
   through `EATASGR` / `EATASGRR` and must match the gate state and gate time
   that were written. Added with #539, when the driver turned out to be writing
   gate states into `EATASGL0` -- whose field is the entry ADDRESS -- while
   returning `k_ra8_ok` on every call.
3. **That every shaper call returned `k_ra8_ok`.** This half proves only that the
   arguments were accepted and the writes were issued. ETHA stays in CONFIG mode,
   so **no frame is ever transmitted and nothing about shaped egress is
   measured.**

The shaper values (window duration, cycle time, CBS increment and upper limit)
are illustrative round numbers; the point is exercising the programming path, not
a specific traffic profile. HUM references: Ch 32 "Ethernet Agent (ETHA)" --
EATASC / EATASGL (TAS), EACAEC / EACAIVC / EACAULC (CBS). The example adds no
raw MMIO of its own.

## Blocked on

Nothing off-target models the ETHA shapers or the GPTP timer -- both windows fall
to the sparse config-reflect fallback, where a counter cannot advance -- so the
app would correctly report failure under emulation. The EK-RA8D2 Ethernet wire is
also marginal (#21).

A measurement peer is provisioned on the bench (`linuxptp` on the HIL Pi's
built-in Ethernet port, which carries a real PTP hardware clock), and it still
does not unblock this app. #292 recorded two firmware blockers; one is closed and
one is not. **Closed:** the schedule reference is now started, because #498
rewrote `ra8_eth_gptp_init` off an invented register window and onto the real
HUM Ch 35 map. **Open:** nothing is transmitted -- the port never leaves CONFIG
mode, no queue is opened, and no frame is queued, and a shaper with no egress
produces nothing to measure. Promotion needs this app extended to actually
transmit, which is not a bench change.
