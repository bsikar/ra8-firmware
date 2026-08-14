# scripts/emu -- EIL vs HIL, and the three entry points in here

EIL is emulator-in-the-loop; HIL is hardware-in-the-loop. They run the **same
`.elf` against the same assertions**: there is no `eil.conf`, both suites source
`scripts/hil/lib/hil_conf.sh` and read each app's `hil.conf`.

**When they disagree, silicon wins.** The emulator is required to reproduce
actual RA8D2 behaviour including its bugs, so an EIL/HIL split is an emulator
modelling defect to fix -- never a reason to relax the assertion.
`scripts/checks/check_hil_eil_parity.py` keeps the two corpora in step: HIL's app
set must be a subset of EIL's, and every `HIL_MODE` must be EIL-checkable. EIL
legitimately runs a superset (the RA8P1 apps no bench can flash); HIL
legitimately asserts modes EIL cannot (the USB host-enumeration ones need a real
peer).

Three scripts live here and answer different questions:

| Script | Asks |
|---|---|
| `eil_all.sh` | does each app still satisfy its own `hil.conf` contract? |
| `matrix.sh` | does **every** example still build and boot at all? (breadth, ratcheted) |
| `smoke.sh` | do the curated display/UI apps still render correctly? (depth) |

**Which do I use?** Adding an assertion about one app's behaviour -> its
`hil.conf`, which `eil_all.sh` then enforces on both sides. Only reach for
`matrix.sh` or `smoke.sh` when the question really is tree-wide breadth or
curated render depth.

<!-- disambig
this: scripts/emu
that: scripts/hil
that: scripts/checks/check_hil_eil_parity.py
symbol: hil_conf_load
symbol: hil_discover_apps
symbol: HIL_EMU_ARGS
files: examples/ek_ra8d2/hw_validated/hil/*/hil.conf = 117
files: scripts/emu/eil_all.sh = 1
files: scripts/emu/matrix.sh = 1
files: scripts/emu/smoke.sh = 1
-->
