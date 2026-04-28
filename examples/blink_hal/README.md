# blink_hal

LED-blink demo built on the project's HAL libraries (`libs/ra_hal/` +
`libs/ra_core/`). Same observable behaviour as `examples/blink/` --
1 Hz toggle on the EK-RA8D2 user-LED candidates -- but using
`ra_gpio_*` and `ra_time_*` instead of raw register pokes.

## Why two blink demos?

| | `blink` | `blink_hal` |
|---|---|---|
| GPIO programming | direct PFS / PCNTR1 writes | `ra_gpio_output_init`, `ra_gpio_toggle` |
| Delay | inline SysTick poll | `ra_time_init` + `ra_delay_ms` |
| Pin validator | bypassed | claimed via the HAL |
| Diagnostic value | "is the chip alive?" | "is the HAL stack alive?" |
| Lines of C | ~250 | ~150 |

Use `blink` first when bringing up a new board or after a major boot
change -- it has the smallest possible surface that can fail. Use
`blink_hal` once `blink` works to verify the HAL libraries link and
run cleanly.

## Build + flash

```sh
make example-blink_hal     # cross-compile
make flash                 # flash via on-board J-Link OB
```

## What it touches

- **`libs/ra_core/src/ra_time.c`** -- SysTick-driven 1 ms tick + busy-wait.
- **`libs/ra_hal/src/ra_gpio.c`** -- PFS unlock dance, pin-validator
  reservation, output-mode programming, atomic toggle via PCNTR3.
- **`libs/ra_core/src/ra_pin_validator.c`** -- pulled in transitively
  to claim each LED pin so the HAL refuses double-assignment.

The HAL stack adds ~24 KB of .text vs. the raw `blink` example (91 KB
total .elf vs. 67 KB), most of which is the pin validator's static
table + log helpers + assertion strings.

## What it deliberately doesn't touch

- **`ra_cgc_init()`** -- PLL bring-up to 1 GHz. The CGC driver writes
  several PRCR-protected registers and HardFaults today on the bare
  chip; we run on the reset-default MOCO ~8.4 MHz.
- **`ra_infrastructure_init()`** -- log + stack canary + pin-validator
  bootstrap. Skipped to keep the demo's failure surface as small as
  possible while still proving the HAL works.
- TrustZone / SAU programming.

When those subsystems are ready to flip on, this example is the
canonical place to add them one at a time.
