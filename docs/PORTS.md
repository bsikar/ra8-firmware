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

"Coupled" means portable code reaches a concrete `libs/ra8_hal/` symbol
directly, with no neutral header in between. The coupling counts are files
under `examples/`; they are a coupling indicator, not a work estimate, and
several files name more than one peripheral.

Every count below is measured, not pinned to a commit and not hand-run. The
manifest in the next section names the command behind each one, and
`scripts/checks/check_measured_counts.py` re-runs all of them, so a number here
that has drifted from the tree fails the gate rather than quietly misleading
the next reader.

| Port | Status | Coupled example files | Priority | Note |
| --- | --- | ---: | --- | --- |
| `fw_os` (mutex / time / yield) | three reinventions, no port | -- | P0 | Splits to its own child issue; do first. |
| clock / CGC | coupled, no port | 227 | P0 | Wants an intent API, not a portable register API. |
| display / framebuffer | facade exists, caps, two backends | 22 | P0 | Enforcement plus backends; see the population rows below. |
| GPIO | coupled, free functions only | 49 | P0 | Extract the vtable; the board owns the pin map. |
| timebase / monotonic `now()` | non-injectable singleton in `libs/ra8_core/` | 237 | P1 | Foundational. `ra8_time_interface.h` is the nearest thing today. |
| timer / counter / capture | coupled | 13 GPT, 3 AGT | P1 | Split from PWM. |
| PWM | coupled to GPT output | -- | P1 | Duty semantics; its own port. |
| serial (full-duplex UART) | output half done via stream | 27 | P1 | The receive half and baud / flow control are the gap. |
| SPI bus, I2C bus, blockdev, stream | **done** -- reference implementations | -- | P3 | Copy the caps discipline from here. |
| filesystem | **done** -- `libs/if/` | -- | P3 | The only port under `libs/if/` today. |
| flash / NVM, ADC, RTC, watchdog | coupled | 4 flash, 2 ADC, 6 RTC, 6 watchdog | P2 | Want `_get_caps()` and `_power()`; re-seat dfu and devcfg. |
| DMA-intent, IRQ-controller, reset | coupled | 6 DMA, 13 ICU/ELC | P2 | Intent only. The two-level RA8 routing does not generalize. |
| SPI-device, CAN, audio, camera | gap or niche | -- | P3 | Design when a board needs one. |
| crypto / RNG | `libs/ra8_psa_crypto/` exists | -- | P2 | PSA Crypto is the one standard adopted as a port. |

Two figures the rows above are read against:

| Measured population | Files |
| --- | ---: |
| `examples/` C and header files | 452 |
| `examples/` files naming `ra8_display_pal` | 22 |

### How the numbers were measured

Every figure above is an entry below: the table row it backs, the count it
claims, and the command that produces it. `scripts/checks/check_measured_counts.py`
re-runs each command against the tree and fails when the count here, or the
cell it names, has drifted. Add a row to a table and add its entry here; there
is no third place to keep in step.

```sh
# MEASURED BLOCK -- re-run by scripts/checks/check_measured_counts.py
# clock / CGC -- 227 file(s)
grep -rlE 'ra8_cgc' examples --include=*.c --include=*.h | wc -l
# display / framebuffer -- 22 file(s)
grep -rlE 'ra8_glcdc|ra8_epaper|ra8_drw' examples --include=*.c --include=*.h | wc -l
# GPIO -- 49 file(s)
grep -rlE 'ra8_gpio|ra8_ioport' examples --include=*.c --include=*.h | wc -l
# timebase / monotonic now() -- 237 file(s)
grep -rlE 'ra8_systick|ra8_time' examples --include=*.c --include=*.h | wc -l
# timer / counter / capture [GPT] -- 13 file(s)
grep -rlE 'ra8_gpt' examples --include=*.c --include=*.h | wc -l
# timer / counter / capture [AGT] -- 3 file(s)
grep -rlE 'ra8_agt' examples --include=*.c --include=*.h | wc -l
# serial (full-duplex UART) -- 27 file(s)
grep -rlE 'ra8_sci|ra8_uart' examples --include=*.c --include=*.h | wc -l
# flash / NVM, ADC, RTC, watchdog [flash] -- 4 file(s)
grep -rlE 'ra8_flash|ra8_nvm' examples --include=*.c --include=*.h | wc -l
# flash / NVM, ADC, RTC, watchdog [ADC] -- 2 file(s)
grep -rlE 'ra8_adc' examples --include=*.c --include=*.h | wc -l
# flash / NVM, ADC, RTC, watchdog [RTC] -- 6 file(s)
grep -rlE 'ra8_rtc' examples --include=*.c --include=*.h | wc -l
# flash / NVM, ADC, RTC, watchdog [watchdog] -- 6 file(s)
grep -rlE 'ra8_wdt|ra8_iwdt' examples --include=*.c --include=*.h | wc -l
# DMA-intent, IRQ-controller, reset [DMA] -- 6 file(s)
grep -rlE 'ra8_dmac|ra8_dtc' examples --include=*.c --include=*.h | wc -l
# DMA-intent, IRQ-controller, reset [ICU/ELC] -- 13 file(s)
grep -rlE 'ra8_icu|ra8_elc' examples --include=*.c --include=*.h | wc -l
# examples/ C and header files -- 452 file(s)
find examples \( -name '*.c' -o -name '*.h' \) -type f | wc -l
# examples/ files naming ra8_display_pal -- 22 file(s)
grep -rlE 'ra8_display_pal' examples --include=*.c --include=*.h | wc -l
```

Three things worth stating plainly, because the figures move:

1. These are *file* counts naming a concrete symbol, not call-site counts. A
   file that calls `ra8_cgc_*` forty times counts once.
2. They differ from the figures quoted when #693 was filed (494 clock, 227
   display, 158 GPIO). Those were call-site counts over a wider file set. The
   ratio between ports is the durable signal; the absolute number is not.
3. The flash, ADC, RTC, watchdog, DMA and interrupt-controller rows carried
   numbers no documented command reproduced. They now carry the pattern that
   produces them, which is why some moved: the watchdog row counts both `wdt`
   and `iwdt`, and the flash row counts what `ra8_flash` and `ra8_nvm`
   actually name.

The display row and the `ra8_display_pal` population row carry the same count.
The port exists and is used; the concrete reach-ins sit alongside it, not
instead of it. That is an enforcement problem, not a missing-port problem.

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
- The gate exists and runs in CI as `gate_agnostic_registers`
  (`scripts/checks/check_agnostic_registers.py`), ratcheted against
  `.github/agnostic-register-baseline.txt`, which may only shrink. So the
  reach-in counts can no longer grow, and criterion 5 is measurable per
  peripheral today: `--list` prints every reference tagged with its family.
  What is still missing for criterion 5 is not the gate but the ports; a
  family cannot reach zero until the neutral seam it would move to exists.
- The gate covers four families (clock, display, GPIO, timer). The other rows
  in the table above are still a grep, recorded here with the command so the
  next person gets the same number.
