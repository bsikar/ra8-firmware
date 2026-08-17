# examples/ek_ra8d2/hw_validated/

Apps confirmed working on a stock EK-RA8D2, filed by **which harness can verify
them**, not by how mature they are.

| | Verified by |
|---|---|
| [`hil/`](hil/) | The app's own `hil.conf`: UART scrape, J-Link memprobe, RTT, USB class binding or a wire-side TCP probe. `make hil-all` on the bench, and the same manifests again in the emulator via `make eil-all`. |
| [`c6/`](c6/README.md) | The same, on the ESP32-C6 bench configuration. `make hil-c6`. |
| [`manual/`](manual/) | Build only -- acceptance is a human observation, a picture on the LCD or a file copied from a PC. |

`make <appname>` from the repo root builds any of them; the bare name works
whichever subdir the app lives in.

Promoting an app is a `git mv` here plus a `hil.conf` beside its `main.c`.
`hil/` is the default and carries an obligation: `check_hil_eil_parity.py`
requires every app in it to be exercised by `ra8_emulator` too, in a mode the
EIL suite can assert, with no skips -- which is what stops a new HIL app from
quietly escaping emulator coverage, and why an app whose peripheral the emulator
does not model cannot simply be dropped in.

`c6/` is separate for a bench reason, not a maturity one: C6 apps need SW4-4
OFF, which takes the Arduino and mikroBUS connectors off the board for every
app in the default pass, so one run cannot serve both -- and `ra8_emulator`
models no C6 (#494), so the parity obligation above cannot be met yet.
