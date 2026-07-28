# examples/ek_ra8d2/hw_validated/

Apps here have been confirmed working on a stock EK-RA8D2 board. They are split
by **which harness can verify them**, not by how mature they are:

| Subdir | How it is verified | Runner |
|--------|-------------------|--------|
| [`hil/`](hil/) | Flash + assert against the app's own `hil.conf` -- UART scrape, J-Link memprobe, RTT, USB class binding or a wire-side TCP probe | `make hil-all`, and the same manifests again in the emulator via `make eil-all` |
| [`c6/`](c6/README.md) | The same, but on the ESP32-C6 bench configuration | `make hil-c6` |
| [`manual/`](manual/) | Build only; running it needs a human at the board or a PC on the far end of a cable | -- |

To build any app: `make <appname>` from the repo root. The bare app name works
regardless of which subdir it lives in.

## Which subdir a promoted app belongs in

`git mv` it here and drop a `hil.conf` beside its `main.c`; discovery is the
filesystem, so nothing else needs editing.

- **`hil/`** is the default, and carries an obligation: `check_hil_eil_parity.py`
  requires every app here to be exercised by `ra8_emulator` too, in a mode the
  EIL suite can check, with no skips. That is what stops a new HIL app from
  quietly escaping emulator coverage -- and it is why an app whose peripheral the
  emulator does not model cannot simply be dropped in.
- **`c6/`** is for the ESP32-C6 companion radio. Those apps need SW4-4 OFF, which
  takes the Arduino and mikroBUS connectors off the board for every app in the
  default pass, so one run of the bench cannot serve both; and `ra8_emulator`
  models no C6 (#494), so the parity obligation above cannot be met yet. Same
  runner, separate lane. See [`c6/README.md`](c6/README.md).
- **`manual/`** is the honest home for anything whose acceptance is a human
  observation -- a picture on the LCD, a file copied from a PC over USB.
