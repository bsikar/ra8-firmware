# eth_hal_bringup

Drives the two chip-generic ETH HAL primitives extracted from the EK-RA8D2
board Ethernet bring-up in issue #581, directly instead of through
`ra8_board_ethernet_init`, logging each step's status over SCI8 (PD_02 / PD_03
-> J-Link OB CDC).

Before #581 the ESWM/COMA media bring-up (RCEC / CABPIRM / MIICR1 / MIIRR) was
open-coded inside `libs/ra8_board_ek_ra8d2/src/ra8_board_ek_ra8d2_ethernet.c`.
It is chip-generic, so it now lives in the HAL:

- `ra8_eth_coma_bringup()` (`ra8_eth_coma`) -- pulses COMA.RRC, enables the
  switch clock (RCEC.RCE), kicks the CABPIRM buffer-pool init and polls
  CABPIRM.BPR, then fans every per-agent clock out (RCEC.ACE[6:0]). Until this
  runs the per-port RMAC / ETHA register windows read back 0.
- `ra8_eth_rgmii_select(port)` (`ra8_eth`) -- selects RGMII on a port's ESWM
  media mux (MIICR = TXCIDE | RGMII) and releases the per-port block reset
  (MIIRR.RGRST = 1, an *enable*, not an active-high reset). This app selects
  port 1 -- the EK-RA8D2 RJ45 is wired to RMAC1 / ETHA1.

The prerequisites the primitives document -- ESWCLK up and the ESWM
module-stop gate released -- are established first with `ra8_cgc_eswclk_init`
and `ra8_mstp_enable(k_ra8_mstp_eswm)`.

Console output:

    eth-hal: boot
    eth-hal: eswclk_init rc=0
    eth-hal: mstp_eswm rc=0
    eth-hal: coma_bringup rc=0
    eth-hal: rgmii_select rc=0
    eth-hal: bringup PASS

`eth-hal: bringup PASS` prints only when every call returned `k_ra8_ok` (`rc=0`).

HUM references: Ch 31 "Ethernet Common Agent (COMA)" (RRC / RCEC / CABPIRM) and
Ch 29 "Layer 3 Ethernet Switch Module (ESWM)" (MIICR1 / MIIRR). The example
adds no raw MMIO of its own; every register access lives behind the HAL API,
which carries the citations.

## Tier: hw_pending (bench-only)

eth is HW-blocked on this board -- the EK-RA8D2 Ethernet wire is marginal
(#21) -- so this app is compile-gated and bench-only and makes no claim of
hardware validation. The COMA/RGMII register accesses are host-tested in
`tests/test_ra8_eth_coma.c` (`coma bringup` happy path + CABPIRM.BPR timeout)
and `tests/test_ra8_eth.c` (`rgmii_select` port 0 / port 1 / bad-port).

Kept **alongside** `../../hw_validated/hil/eth_open_probe`, which drives the
full board bring-up plus `ra8_eth_open`; this app is the minimal witness that
the two extracted primitives compose on their own (COMA out of reset -> media
mux selects RGMII). It does not assert a link is up, which needs the corrected
RGMII skew and a peer.

Build:

```
make eth_hal_bringup
```
