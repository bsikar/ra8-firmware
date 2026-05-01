# tools/ra_qe -- JSON-driven configurator

Hand-rolled, command-line equivalent of the Renesas QE / SCFG configurator
tool. The standing rule for this project is "no e2 studio, ever": all builds
are CMake + arm-none-eabi-gcc, and any vendor-style code generation needs a
plain Python implementation that fits the same workflow.

## What it does

Reads a JSON document describing the four things a typical RA8D2 bring-up
config XML covers:

- `clocks`  -- primary clock source, derived bus frequencies (CPUCLK0, ICLK,
              PCLKA, PCLKB).
- `pinmux`  -- ordered list of pin configurations (port, pin, mode,
              initial level, optional symbolic label).
- `irqs`    -- ICU vector slot, NVIC priority, ISR symbol.
- `mstp`    -- module-stop releases (e.g. release IOPORT before driving pins).

Validates the document against `schema/config.schema.json` (built-in walker;
no external `jsonschema` package), then emits four hand-written-style C23
source/header pairs:

```
gen_clocks.{c,h}   ra_cgc_init() hook + clock-tree typed enums
gen_pinmux.{c,h}   ra_gpio init table
gen_irq.{c,h}      ICU vector table entries
gen_mstp.{c,h}     module-stop release calls
```

All generated code conforms to CLAUDE.md style:

- `#pragma once` headers
- Full file-level Doxygen blocks
- C23 typed enums (`typedef enum : uint8_t { ... } name_t;`) for every
  integer constant
- Statically allocated tables -- NASA Rule 3 compliant, zero malloc
- ASCII-only

## Usage

```sh
python3 tools/ra_qe/generate.py \
    --config tools/ra_qe/examples/blink_config.json \
    --output-dir build/gen/blink
```

After generation, an example app can `#include "gen_pinmux.h"` and walk
`g_gen_pinmux_table[]` from its `main()` to bring up its pin configuration.

## Schema

See `schema/config.schema.json`. Top-level required keys: `app`, `clocks`,
`pinmux`, `irqs`, `mstp`. The validator supports the JSON Schema subset we
need (type, enum, required, properties, additionalProperties, items,
minimum, maximum, pattern) -- enough for the DSL without pulling in a pip
dependency.

## Tests

`tools/ra_qe/tests/test_generate.py` runs the generator on the sample config
and, if `arm-none-eabi-gcc` is installed, compiles every generated `.c` with
`-c -std=c23 -Wall -Wextra -Werror` to confirm the output is clean. The test
skips the compile step gracefully when the cross-compiler is not present.

The top-level `make qe-test` target runs the unittest module.
