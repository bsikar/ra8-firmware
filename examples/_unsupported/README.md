# examples/_unsupported/

Apps that cannot be hardware-validated on a stock **EK-RA8D2 v1**: each needs
extra hardware, a vendor binary blob, or a Renesas add-on board this project
does not own. They are kept as reference implementations, and they still have
to cross-compile and satisfy every static gate -- but nothing in CI flashes
them, so expect bit-rot that a refactor will not catch. Each app's own README
says what it is waiting for.

A new hardware-dependent app belongs here, so the next person scanning the tree
can tell at a glance what can and cannot be validated on the stock board. Apps
that need no extra hardware live in [`../ek_ra8d2/`](../ek_ra8d2/README.md),
whose tiers distinguish recorded evidence from current-candidate claims.

## Every app here states why, in a form a gate can read

Each app carries an `UNSUPPORTED.toml` marker naming its exclusion reason from a
bounded taxonomy, the file that evidences it, and whether it needs hardware this
project does not own. `scripts/checks/check_unsupported_exclusions.py` proves
the markers and the app directories are the same set in both directions, so an
app cannot be parked here silently and a marker cannot outlive its app.

| reason | means |
| --- | --- |
| `external-hardware` | needs a part or add-on board this project does not own |
| `companion-radio` | needs the ESP32-C6 companion carrying a radio the RA8D2 lacks |
| `host-side-rig` | needs a workstation-side setup (network bridge, host driver, credential) |
| `onboard-routing-conflict` | the pins exist on the stock board but are jumpered to another peripheral |
| `removable-media` | needs a card or medium a person has to insert |
| `human-observation` | the pass/fail can only be judged by a person (a tone heard, a stream rendered) |

The reason is a statement of what the app is waiting for, not a verdict that the
exclusion is still correct. Two of the six do not in fact need hardware this
project lacks (`needs_extra_hardware = false`), which is exactly the kind of
stale tiering a bench pass should revisit; see #401.
