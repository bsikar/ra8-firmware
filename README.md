# ra8d2-firmware

Bare-metal firmware for the Renesas **RA8D2** MCU group, targeting the
**EK-RA8D2** evaluation kit (part number 968-K7EKA8D2S01001BE).

## Target

| | |
|---|---|
| MCU | `R7KA8D2KFLCAC` |
| Primary core | Arm Cortex-M85 @ 1 GHz (Helium / MVE) |
| Secondary core | Arm Cortex-M33 @ 250 MHz |
| Code memory | 1 MB MRAM |
| SRAM | 2 MB (ECC) |
| Package | 289-BGA (12 mm x 12 mm, 0.65 mm pitch) |
| Debugger | On-board SEGGER J-Link OB (SWD) |

## Approach

- **Bare-metal**, hand-written HAL on top of the chip's register map.
- No Renesas FSP code is checked in. FSP / CMSIS-DFP / FSP BSP docs may be
  read for reference, but every `.c` / `.h` in `src/` and `libs/` is
  hand-written under the rules in [CLAUDE.md](CLAUDE.md).
- No vendor IDE project files (`.cproject`, `.uvprojx`, etc.). CMake only.

## Build

```bash
# Prerequisites:
#   - CMake >= 3.20
#   - ARM GNU Toolchain (arm-none-eabi-gcc) in PATH
#   - clang-format and clang-tidy (>= 16) for lint checks

./build.sh                 # Debug build (default)
./build.sh release         # Optimized build
./build.sh clean           # Clean rebuild
./build.sh -v              # Verbose
```

Artifacts land in `build/`:

```
build/ra8d2-firmware.elf    ELF with DWARF symbols
build/ra8d2-firmware.hex    Intel HEX (for flashers)
build/ra8d2-firmware.bin    Raw binary
build/ra8d2-firmware.map    Linker map (sizes + symbol placement)
```

## Lint and format

```bash
bash scripts/format_code.sh            # Format in place
bash scripts/format_code.sh --check    # Check formatting only (no changes)
bash scripts/clang_tidy.sh             # Run clang-tidy
bash scripts/clang_tidy.sh --fix       # Apply clang-tidy auto-fixes
```

## Reference material

Committed under [`docs/reference/`](docs/reference/):

- `ra8d2-datasheet.pdf` -- electrical specs, pin lists
- `ra8d2-hardware-user-manual.pdf` -- primary register reference (use this one)
- `ra8d2-technical-brief.pdf` -- high-level overview
- `ra8d2-high-temperature-operation.pdf` -- hi-temp application note

## License

MIT, Copyright (c) 2026 Brighton Sikarskie. See [LICENSE.txt](LICENSE.txt).
