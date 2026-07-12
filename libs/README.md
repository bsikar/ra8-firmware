# libs/

The project's standard library. Five subdirectories, each one
ring of the layered architecture (see
[`docs/RING_AND_WORLD.md`](../docs/RING_AND_WORLD.md)).

| Dir | Ring | Files | What it is |
|---|---|---|---|
| [`ra8_core/`](ra8_core/) | 1 -- core | 7 c / 18 h | Pure-C utilities with **no hardware dependencies**. Compiles identically on the host (unit tests) and the ARM target. Includes `ra8_err`, `ra8_log`, `ra8_check`, `ra8_time` (SysTick wrapper), pin validator, register-protection guards, error/exception handlers, time + bit + port + GPIO constants. The only layer drivers may freely depend on. |
| [`ra8_hal/`](ra8_hal/) | 2 + 3 -- regs + drivers | 66 c / 124 h | The Hardware Abstraction Layer. Splits into two pieces inside the same directory: <br>&nbsp;&nbsp;`inc/ra8d2_*_regs.h` -- hand-written register layouts derived from the HUM (Ring 2; structs + typed enums + inline accessors, no code paths). <br>&nbsp;&nbsp;`src/ra8_*.c` + `inc/ra8_*.h` -- the actual peripheral drivers (Ring 3; init/deinit/IRQ/DMA paths). Every documented RA8D2 peripheral has a driver here. |
| [`ra8_nsc/`](ra8_nsc/) | 4 -- NSC veneers | 7 c / 4 h | TrustZone Non-Secure-Callable veneers. The **only** place in the tree where `__attribute__((cmse_nonsecure_entry))` is allowed -- the linter rejects it anywhere else. The `.gnu.sgstubs` linker section lands here. Bridges between `{World: S}` HAL code and `{World: NS}` application code. |
| [`ra8_net_pal/`](ra8_net_pal/) | 4 -- PAL | 1 c / 1 h | Ethernet Platform Abstraction Layer. Sits above `ra8_hal/`'s ESWM / ETHA / RMAC drivers and below an external IP stack (NetX Duo today; FreeRTOS+TCP / TCPDirect possible). Ring-buffer-shaped frame I/O so the IP stack doesn't need to know about RA8D2 specifics. |
| [`ra8_usb_pal/`](ra8_usb_pal/) | 4 -- PAL | 1 c / 1 h | USB Platform Abstraction Layer. Sits above `ra8_hal/`'s `ra8_usb.c` (pipe / endpoint primitives) and below an external USB stack (CherryUSB, TinyUSB, etc.). Pipe descriptor model, EP0 control-transfer state machine. |

## Layering rule

Code may depend on anything **lower** in the table; never on
anything higher. Concretely:

```
examples/<app>/main.c             -->  ra8_hal  -->  (ra8d2_*_regs.h)
                            \-->  ra8_core
                            \-->  ra8_net_pal / ra8_usb_pal / ra8_nsc
ra8_hal                     -->  ra8_core
ra8_core                     -->  (nothing in libs/)
```

(`<app>` is any directory under `examples/` containing a `main.c`,
e.g. `examples/ek_ra8d2/hw_validated/hil/blink/`, `.../smoke/blink_hal/` -- see the repo-root
[README](../README.md).)

`ra8_core` exists specifically so `ra8_hal` drivers can rely on
common primitives (error codes, logging, time) without having a
"miscellaneous" pile inside `ra8_hal/`. `ra8_core` itself never
touches hardware -- the unit-test build and the ARM cross-build
compile it from the same sources.

## Adding a new library

If it has hardware dependencies, it's a HAL driver -- add files
under `libs/ra8_hal/`, not a new top-level directory. The four
existing top-level `libs/` entries cover every architectural ring;
new functionality fits inside one of them.
