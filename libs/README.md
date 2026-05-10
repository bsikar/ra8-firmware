# libs/

The project's standard library. Five subdirectories, each one
ring of the layered architecture (see
[`docs/RING_AND_WORLD.md`](../docs/RING_AND_WORLD.md)).

| Dir | Ring | Files | What it is |
|---|---|---|---|
| [`ra_core/`](ra_core/) | 1 -- core | 7 c / 18 h | Pure-C utilities with **no hardware dependencies**. Compiles identically on the host (unit tests) and the ARM target. Includes `ra_err`, `ra_log`, `ra_check`, `ra_time` (SysTick wrapper), pin validator, register-protection guards, error/exception handlers, time + bit + port + GPIO constants. The only layer drivers may freely depend on. |
| [`ra_hal/`](ra_hal/) | 2 + 3 -- regs + drivers | 66 c / 124 h | The Hardware Abstraction Layer. Splits into two pieces inside the same directory: <br>&nbsp;&nbsp;`inc/ra8d2_*_regs.h` -- hand-written register layouts derived from the HUM (Ring 2; structs + typed enums + inline accessors, no code paths). <br>&nbsp;&nbsp;`src/ra_*.c` + `inc/ra_*.h` -- the actual peripheral drivers (Ring 3; init/deinit/IRQ/DMA paths). Every documented RA8D2 peripheral has a driver here. |
| [`ra_nsc/`](ra_nsc/) | 4 -- NSC veneers | 7 c / 4 h | TrustZone Non-Secure-Callable veneers. The **only** place in the tree where `__attribute__((cmse_nonsecure_entry))` is allowed -- the linter rejects it anywhere else. The `.gnu.sgstubs` linker section lands here. Bridges between `{World: S}` HAL code and `{World: NS}` application code. |
| [`ra_net_pal/`](ra_net_pal/) | 4 -- PAL | 1 c / 1 h | Ethernet Platform Abstraction Layer. Sits above `ra_hal/`'s ESWM / ETHA / RMAC drivers and below an external IP stack (lwIP, FreeRTOS+TCP, etc.). Ring-buffer-shaped frame I/O so the IP stack doesn't need to know about RA8D2 specifics. |
| [`ra_usb_pal/`](ra_usb_pal/) | 4 -- PAL | 1 c / 1 h | USB Platform Abstraction Layer. Sits above `ra_hal/`'s `ra_usb.c` (pipe / endpoint primitives) and below an external USB stack (CherryUSB, TinyUSB, etc.). Pipe descriptor model, EP0 control-transfer state machine. |

## Layering rule

Code may depend on anything **lower** in the table; never on
anything higher. Concretely:

```
examples/<app>/main.c             -->  ra_hal  -->  (ra8d2_*_regs.h)
                            \-->  ra_core
                            \-->  ra_net_pal / ra_usb_pal / ra_nsc
ra_hal                     -->  ra_core
ra_core                     -->  (nothing in libs/)
```

(`<app>` is any directory under `examples/` containing a `main.c`,
e.g. `examples/ek_ra8d2/hw_validated/smoke/blink/`, `.../smoke/blink_hal/` -- see the repo-root
[README](../README.md).)

`ra_core` exists specifically so `ra_hal` drivers can rely on
common primitives (error codes, logging, time) without having a
"miscellaneous" pile inside `ra_hal/`. `ra_core` itself never
touches hardware -- the unit-test build and the ARM cross-build
compile it from the same sources.

## Adding a new library

If it has hardware dependencies, it's a HAL driver -- add files
under `libs/ra_hal/`, not a new top-level directory. The four
existing top-level `libs/` entries cover every architectural ring;
new functionality fits inside one of them.
