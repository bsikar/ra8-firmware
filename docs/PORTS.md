# Port catalog

A **port** in this tree is a hardware-neutral seam a portable library depends
on, with at least one chip-specific adapter behind it. This page is the
catalog: which ports exist, which are still coupled to `libs/ra8_hal/`, the
shape a new port has to take, and the order the near-term ones get built in.

It is the planning artifact for
[#693](https://github.com/bsikar/ra8-firmware/issues/693) under epic
[#692](https://github.com/bsikar/ra8-firmware/issues/692). Anything that says
"port" in a platform-architecture issue means the shape defined here.

## The shape of a port

The pattern was not imported from embedded-hal or CMSIS. It is the one this
tree converged on in `libs/ra8_io/`, and it has three deliberately separate
pieces. A port picks the pieces it needs.

**(a) The interface / facade.** A caller-allocated opaque handle binding a
private vtable to a `void* ctx`. No heap; the backend state is provided by the
caller. This is the board-time selection mechanism. Reference:
`ra8_io_spi_bus_t` in [`libs/ra8_io/inc/ra8_io_spi_bus.h`](../libs/ra8_io/inc/ra8_io_spi_bus.h),
bound to either [`libs/ra8_io/inc/ra8_io_spi_bus_spi_b.h`](../libs/ra8_io/inc/ra8_io_spi_bus_spi_b.h)
or [`libs/ra8_io/inc/ra8_io_spi_bus_sci_spi.h`](../libs/ra8_io/inc/ra8_io_spi_bus_sci_spi.h).

**(b) The narrow ops struct.** A small function-pointer struct a Ring-3 driver
stores by value and is injected with, so the driver never names a concrete
peripheral. This is the mockable, MC/DC-able seam. Reference:
`ra8_spi_bus_ops_t`, `ra8_i2c_bus_ops_t`.

**(c) The `_as_ops()` bridge.** Fills (b) from a bound (a), so ring ordering is
never inverted. Reference: `ra8_io_spi_bus_as_ops()`, `ra8_io_i2c_bus_as_ops()`.

Which pieces a port earns:

| Situation | Earns |
| --- | --- |
| More than one electrically-equivalent block on one chip (SPI_B vs SCI-SPI) | (a) + (b) + (c) |
| One implementation, many backends (blockdev, display) | (a) only |
| A test-DI need with no runtime selection | (b) only |

Rules that hold for every port:

- The neutral header lives under `libs/if/` (pattern:
  [`libs/if/inc/fw_if_fs.h`](../libs/if/inc/fw_if_fs.h)), includes only
  `libs/ra8_core/`, and states in its `@details` which of (a)/(b)/(c) it uses.
- Every method returns `ra8_err_t` through a `[[nodiscard]]` out-param.
- Every port with heterogeneous backends carries `_get_caps()`, returning an
  immutable snapshot from a complete binding. In-tree today: `fw_fs_get_caps`,
  `display_get_caps`, `ra8_io_blockdev_get_caps`, `ra8_io_vfs_get_caps`.
- The board owns the pins, the clock profile, and the backend selection. The
  port does not.
- Ring and World tags per [`docs/RING_AND_WORLD.md`](RING_AND_WORLD.md): the
  neutral header is Ring 2 / Interface, the adapter is Ring 3 / HAL.

## The catalog

Status as measured against `dev` at `26bb09c`. "Coupled" means portable code
reaches a concrete `libs/ra8_hal/` symbol directly, with no neutral header in
between. The coupling counts are files under `examples/`, measured by the
commands in the next section; they are a coupling indicator, not a work
estimate, and several files name more than one peripheral.

| Port | Status | Coupled example files | Priority | Note |
| --- | --- | ---: | --- | --- |
| `fw_os` (mutex / time / yield) | three reinventions, no port | -- | P0 | Splits to its own child issue; do first. |
| clock / CGC | coupled, no port | 240 | P0 | Wants an intent API, not a portable register API. |
| display / framebuffer | facade exists, caps, two backends | 22 | P0 | Enforcement plus backends; see the bypass count below. |
| GPIO | coupled, free functions only | 52 | P0 | Extract the vtable; the board owns the pin map. |
| timebase / monotonic `now()` | non-injectable singleton in `libs/ra8_core/` | 247 | P1 | Foundational. `ra8_time_interface.h` is the nearest thing today. |
| timer / counter / capture | coupled | 13 GPT, 3 AGT | P1 | Split from PWM. |
| PWM | coupled to GPT output | -- | P1 | Duty semantics; its own port. |
| serial (full-duplex UART) | output half done via stream | 26 | P1 | The receive half and baud / flow control are the gap. |
| SPI bus, I2C bus, blockdev, stream | **done** -- reference implementations | -- | P3 | Copy the caps discipline from here. |
| filesystem | **done** -- `libs/if/` | -- | P3 | The only port under `libs/if/` today. |
| flash / NVM, ADC, RTC, watchdog | coupled | 10 / 2 / 6 / 4 | P2 | Want `_get_caps()` and `_power()`; re-seat dfu and devcfg. |
| DMA-intent, IRQ-controller, reset | coupled | 6 DMA, 13 ICU/ELC | P2 | Intent only. The two-level RA8 routing does not generalize. |
| SPI-device, CAN, audio, camera | gap or niche | -- | P3 | Design when a board needs one. |
| crypto / RNG | `libs/ra8_psa_crypto/` exists | -- | P2 | PSA Crypto is the one standard adopted as a port. |

### How the numbers were measured

Reproduce any row from the repository root:

```sh
grep -rlE 'ra8_cgc' examples --include=*.c --include=*.h | wc -l   # 240
grep -rlE 'ra8_gpio|ra8_ioport' examples --include=*.c --include=*.h | wc -l   # 52
grep -rlE 'ra8_glcdc|ra8_epaper|ra8_drw' examples --include=*.c --include=*.h | wc -l   # 22
grep -rlE 'ra8_systick|ra8_time' examples --include=*.c --include=*.h | wc -l   # 247
```

Out of 470 `.c` / `.h` files under `examples/`. Two caveats worth stating
plainly, because the figures move:

1. These are *file* counts naming a concrete symbol, not call-site counts. A
   file that calls `ra8_cgc_*` forty times counts once.
2. They differ from the figures quoted when #693 was filed (494 clock, 227
   display, 158 GPIO). Those were call-site counts over a wider file set. The
   ratio between ports is the durable signal; the absolute number is not, and
   nothing should gate on it until a checker owns the measurement.

The display row needs one more fact to read correctly: 22 example files name a
concrete display symbol and 22 name `ra8_display_pal`. The port exists and is
used; the concrete reach-ins are alongside it, not instead of it. That is an
enforcement problem, not a missing-port problem.

## Acceptance criteria, per port

A port is done when all five hold. These are the criteria every child issue in
the platform-architecture epic inherits:

1. A neutral `fw_if_<peripheral>.h` exists under `libs/if/`, includes only
   `libs/ra8_core/`, and documents which of (a) / (b) / (c) it uses.
2. At least one chip adapter implements it against the register layer.
3. A port with heterogeneous backends exposes `_get_caps()`.
4. The board owns that peripheral's pins, clock profile, and backend selection.
5. The agnostic-versus-register gate is green for that peripheral's symbols, so
   the reach-ins cannot regrow.

## Build-first order

0. `fw_os` OSAL, and de-middleware the SysTick. Everything else assumes it.
1. **clock**. Top leverage, 240 coupled files. Build a portable *intent* API
   ("module X clocked", "source at or above N Hz for peripheral Y") resolved by
   a board-owned and chip-owned clock profile. Not a portable register API: the
   RX and RISC-V clock trees do not share a shape with this one.
2. **display**. Land the agnostic-versus-register gate first, then migrate the
   reach-ins, then add the parallel-RGB, MIPI-DSI and external-SPI backends.
3. **GPIO**. Extract the vtable; the free functions are already correctly
   error-modelled. Finish moving the pin map into the board.
4. **timebase**. A monotonic `now()` contract. The quiet foundational gap.
5. **timer** and **PWM**, split apart.
6. **serial**, the full-duplex peer of the stream output.

Adopt uniformly as each lands: `_get_caps()`; one completion-callback typedef
plus `k_ra8_err_would_block` for non-blocking work, with no future or executor
machinery; a uniform `_power(state)` per port. Prefer link-time adapter
selection on hot and safety paths, and reserve vtables for runtime polymorphism
and mock injection.

## What is not true yet

Stated so no one reads this page as a description of the tree:

- `libs/if/` holds one port. Every other neutral seam in the table above is
  still to be written.
- There is no agnostic-versus-register gate yet, so criterion 5 cannot pass for
  any peripheral, and the coupling counts can grow between now and then.
- The coupling counts have no checker behind them. They are a grep, recorded
  here with its command so the next person gets the same number.
