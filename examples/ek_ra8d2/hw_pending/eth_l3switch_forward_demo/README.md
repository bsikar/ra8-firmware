# eth_l3switch_forward_demo

Configures the RA8D2 Ethernet frame-forwarding path that no other example
referenced (recon gap #135), logging over SCI8 (PD_02 / PD_03 -> J-Link OB
CDC). LED1 blinks as a heartbeat.

Two drivers are exercised, honestly separated:

- `ra8_eth_mfwd` -- the **real** Message Forwarding engine between the GMAC
  ports and the CPU Agent (GWCA). The app programs the per-port forwarding
  masks (`ra8_eth_mfwd_set_forwarding_masks`, permissive 0x7F on ports 0/1/host),
  routes port-0 ingress into GWCA RX queue 0 (`ra8_eth_mfwd_route_queue`), and
  reads the MFWD status (`ra8_eth_mfwd_get_status`).
- `ra8_layer3_switch` -- the FSP-shaped L3-switch **facade**. The RA8D2 silicon
  carries no L3 switch block (the driver header documents this), so the
  facade's route-table ops are placeholders that return `k_ra8_err_not_supported`.
  The app drives the working lifecycle (`open` / `status_get` / `close`) and
  calls `route_add` once to log -- honestly -- that the L3 route table is a
  not-supported placeholder on this part. The real forwarding config is the
  MFWD path above.

Console output per cycle:

    l3sw: fwd_sts=0x0
    l3sw: l3_open=1 promisc=0
    l3sw: route_add=263 (placeholder)
    l3sw: forward PASS

`route_add=263` is `k_ra8_err_not_supported` (0x107) -- the documented
placeholder, logged but never failing the verdict. `l3sw: forward PASS` prints
only when every real operation (MFWD program + status, facade open / status /
close) returned `k_ra8_ok`.

HUM references: Ch 31-32 "Ethernet" -- MFWD FWPBFC0 (per-port forwarding masks)
and FWPBFCSDC0 (port-to-host queue routing). The example adds no raw MMIO of
its own; every register access lives behind the driver API, which carries the
citations.

## Tier: hw_pending (bench-only)

`tools/ra8_emulator` has no Ethernet / MFWD peripheral model, so there is no
`make sim-` gate for this app; the EK-RA8D2 Ethernet wire is also marginal
(#21). The example's value is demonstrating the real forwarding API behind a
clean ARM cross-build, matching the driver-gap example wave (#182-188).

Proving a frame is actually forwarded to the correct egress port needs a
multi-port topology (two links + a traffic source, bench wiring #89); promote
to `hw_validated/hil/` once forwarding is confirmed on silicon.

Build:

```
make eth_l3switch_forward_demo
```
