# examples/

Each subdirectory is a standalone application that builds against the
shared HAL + boot code in `libs/` and `src/`. Pick one with
`-DEXAMPLE=<name>` at cmake time, or use the convenience Makefile
target:

```sh
make examples           # list every available example
make example-blink      # build examples/blink/
make flash              # flash whichever example was just built
```

Adding a new example is a one-line operation: drop a
`examples/<name>/main.c` in place. CMake's `CONFIGURE_DEPENDS` glob
re-scans on the next build.

## Available examples

| Name | What | Smoke-test depth |
|---|---|---|
| [`blink`](blink/) | 1 Hz LED toggle via raw register writes, no HAL deps | "is the chip alive?" |
| [`blink_hal`](blink_hal/) | Same blink, but built on `ra_gpio_*` + `ra_time_*` | "is the HAL alive?" |

Use `blink` first when bringing up a new board (smallest possible
failure surface). Use `blink_hal` once `blink` works to verify the
HAL libraries link and run cleanly.
