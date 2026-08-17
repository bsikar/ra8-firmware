# eth_l3switch_forward_demo

Configures the RA8D2 Ethernet frame-forwarding path that no other example
referenced (recon gap #135). Two drivers are exercised, honestly separated:

- `ra8_eth_mfwd` -- the **real** Message Forwarding engine between the GMAC
  ports and the CPU Agent (GWCA). The app programs the per-port forwarding
  masks, routes port-0 ingress into a GWCA RX queue, and reads MFWD status.
- `ra8_layer3_switch` -- the FSP-shaped L3-switch **facade**. The RA8D2 silicon
  carries no L3 switch block, so the facade's route-table ops return
  `k_ra8_err_not_supported`. The app drives the working lifecycle (open, status,
  close) and calls `route_add` once purely to log -- honestly -- that the route
  table is a placeholder on this part. That call is never allowed to fail the
  verdict; the real forwarding configuration is the MFWD path above.

HUM references: Ch 31-32 "Ethernet" -- MFWD FWPBFC0 (per-port forwarding masks)
and FWPBFCSDC0 (port-to-host queue routing). The example adds no raw MMIO of its
own.

## Why this one cannot be promoted from this board

Proving a frame is actually forwarded to the correct egress port needs **two
links**. The RA8D2 has two ETHA/RMAC ports, but the EK-RA8D2 breaks out a single
RJ45 with one populated PHY, so there is no second port to observe an egress on.
That is a board limitation, not a bench-configuration one: no cabling, peer or
instrument can make end-to-end forwarding assertable here. It needs the carrier
PCB (#318) that breaks out the second port, or a second board. (An earlier
revision blamed bench wiring #89 and sent the work to the wrong queue -- #89 is a
bench task and would never have unblocked this.)

There is also no Ethernet or MFWD peripheral model off-target, and the EK-RA8D2
Ethernet wire is marginal (#21).
