# net_pal_frame_demo

First application consumer of `libs/ra8_net_pal/`, the Ring-4 ethernet PAL.

## Why this exists

`ra8_net_pal` is the stack-facing seam: it owns the MAC address, wraps the
Ring-3 `ra8_eth` driver (ESWM module-stop gate, status fan-out), and gives a
network stack one send primitive, one receive primitive, a link query and one
event hook.

Nothing in `apps/` or `examples/` ever included `ra8_net_pal.h`. Its only
in-tree callers were:

- `libs/ra8_nsc/src/ra8_nsc_eth.c`, the TrustZone veneers, which no
  application calls either; and
- the host tests under `tests/net/`.

NetX Duo's driver (`port/netxduo/src/nx_ether_driver_ra8_eth.c`) deliberately
bypasses the PAL and calls `ra8_eth_*` directly (#621), so on a board build the
PAL was compiled and never brought up. This app is the first consumer that
initialises it on hardware and drives the whole documented surface.

## What it does

Seven legs, one verdict line each:

| Leg            | Checks                                                             |
| -------------- | ------------------------------------------------------------------ |
| `setup`        | CGC and the MSTP controller come up: the PAL's precondition, since |
|                | `ra8_net_pal_init` releases the ESWM gate through `ra8_eth_init`.  |
| `bind`         | Init programmes the supplied MAC, it reads back byte for byte, and |
|                | a fresh PAL reports the link down.                                 |
| `mac`          | A second MAC replaces the first; both accessors refuse `NULL`.     |
| `ring`         | 64-byte, 128-byte and 1518-byte (`k_ra8_net_pal_frame_max`) frames |
|                | round trip in FIFO order, byte-verified with lengths preserved.    |
| `backpressure` | The ring depth is measured, not restated: send until the PAL says  |
|                | `k_ra8_err_no_mem`, refusal repeats while full, draining one slot  |
|                | makes room, and an empty ring reports `k_ra8_err_no_data`.         |
| `guards`       | Zero length, one byte past the frame ceiling, a `NULL` frame,      |
|                | `NULL` receive arguments, an undersized receive buffer and a       |
|                | `NULL` link pointer each get the documented code, and the ring is  |
|                | then shown to be as empty as it was on entry.                      |
| `events`       | An installed handler sees exactly one `tx_done` per accepted send  |
|                | (a refused send raises nothing), the context pointer comes back    |
|                | intact, and detaching stops delivery.                              |
| `unwind`       | After `ra8_net_pal_deinit` every entry point reports               |
|                | `k_ra8_err_invalid_state`, including a second deinit.              |

## No hardware needed beyond the board

The PAL's frame ring is RAM-backed today; the GWCA descriptor engine lands with
the real media path. So the round trip is a genuine loopback of the
stack-facing API, not wire traffic: no PHY, no cable and no link partner. The
one hardware touch is the ESWM module-stop release inside `ra8_eth_init`.

## Build and run

```sh
cmake --preset ra8d2-debug
ninja -C cmake-build-debug net_pal_frame_demo.elf
```

Flash and watch SCI8 (J-Link OB VCOM, 115200 8N1). A good run ends with:

```
net_pal_frame_demo: ALL PASS
```

Any failing leg prints `net_pal_frame_demo: <leg> FAIL` and the summary line
becomes `ALL FAIL`.

## Status

`hw_pending`: compiles against the pinned arm-gnu-toolchain 13.3.rel1 and is
fully self-checking, but has not been run on a bench board yet.
