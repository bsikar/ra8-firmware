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

- **blink** -- 1 Hz LED toggle on the EK-RA8D2, SysTick-driven, no
  HAL dependencies. Use as a "is the board alive" smoke test and as
  the template for new examples.
