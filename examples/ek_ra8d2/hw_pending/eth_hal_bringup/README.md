# eth_hal_bringup

Drives the two chip-generic ETH HAL primitives extracted from the EK-RA8D2 board
Ethernet bring-up (#581) directly, instead of through
`ra8_board_ethernet_init`, logging each step's status. Before #581 the ESWM/COMA
media bring-up was open-coded inside the board library; it is chip-generic, so it
now lives in the HAL.

- `ra8_eth_coma_bringup()` pulses COMA.RRC, enables the switch clock (RCEC.RCE),
  kicks the CABPIRM buffer-pool init and polls CABPIRM.BPR, then fans every
  per-agent clock out (RCEC.ACE[6:0]). **Until this runs, the per-port RMAC and
  ETHA register windows read back 0.**
- `ra8_eth_rgmii_select(port)` selects RGMII on a port's ESWM media mux
  (MIICR = TXCIDE | RGMII) and releases the per-port block reset -- note that
  `MIIRR.RGRST = 1` is an *enable*, not an active-high reset. This app selects
  port 1, because the EK-RA8D2 RJ45 is wired to RMAC1 / ETHA1.

The prerequisites those primitives document -- ESWCLK up and the ESWM
module-stop gate released -- are established first. HUM references: Ch 31
"Ethernet Common Agent (COMA)" for RRC / RCEC / CABPIRM, and Ch 29 "Layer 3
Ethernet Switch Module (ESWM)" for MIICR1 / MIIRR. The example adds no raw MMIO
of its own.

It is kept **alongside** `eth_open_probe`, which drives the full board bring-up
plus `ra8_eth_open`; this app is the minimal witness that the two extracted
primitives compose on their own -- COMA out of reset, then the media mux selects
RGMII. It does **not** assert a link is up, which would need the corrected RGMII
skew and a peer.

## Accepted hardware limitation

The full-link path remains constrained by the accepted EK-RA8D2 transmit
limitation characterized in closed issue #21. The issue was closed because the
MTU=128 mitigation is sufficient for this project and the e-reader does not use
this Ethernet port, not because full-size frames were repaired. The COMA and
RGMII register paths are host-tested instead
(`tests/misc/src/test_ra8_eth_coma.c` covers the bring-up happy path and the CABPIRM.BPR
timeout; `tests/misc/src/test_ra8_eth.c` covers `rgmii_select` per port and the bad-port
rejection).
