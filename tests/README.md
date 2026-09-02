# tests/

Host-side tests, compiled with the host toolchain and never cross-compiled --
the target build is the top-level `CMakeLists.txt`. `just quality::local::test` runs them.

`tests/CMakeLists.txt` registers one executable per ordinary `test_*.c` under
each category's `src/` directory and per app-local C test under
`apps/shared_libs/*/tests/` or `apps/board/stand_alone/*/tests/`. Example-local
tests, tests needing a special link or device gate, and C++ tests are instead
defined in one of the `tests/cmake/tests_*.cmake` fragments and excluded from the
ordinary glob. The runner
enforces a shrink-only floor of 691 registered tests so a moved directory
cannot silently reduce the suite again. The Alphabet Soup `/proc/self/mem` and
closed-stdout cases remain registered but disabled off Linux, preserving the
cross-host inventory without claiming that those Linux-specific paths execute
on macOS.

The `tests/` root contains only its CMake and shell entry points. Every compiled
test category uses the same layout: implementation and test translation units
live in `<category>/src/`, while authored headers live in `<category>/inc/`.
Targets include those `inc/` directories and use header basenames, so source
files do not depend on the repository's physical directory depth.

The deliberate exemptions are non-code assets and build machinery. Fixture
payloads stay below `fixtures/`, fuzz corpora stay below `fuzz/corpus/`, pinned
renders stay below `golden/`, and CMake fragments stay below `cmake/`. Those
files are inputs or configuration rather than compiled source/header units.

## Subdirectories

The `tests-readme` gate (`scripts/checks/check_tests_readme.py`) reads the first
cell of each row below and fails if it drifts from the tree in either direction
-- an undescribed subdirectory, or a row whose subdirectory is gone.

| Subdirectory | What it holds |
|---|---|
| `bench/` | Host microbenchmarks under `src/`, not correctness tests: one `bench_ra8_*.c` per subject over the hand-written `bench/inc/ra8_bench.h` harness, each printing a CSV `name,iterations,ns_per_op,MB_per_s` row. |
| `cmake/` | The fragments that assemble the host build, included in order: `host_config` (standards, warnings, coverage / MC/DC / sanitizer modes), `unit_tests` (the auto-glob and `ra8_add_test()`), `library_sources`, `core_hal`, then the themed `tests_*.cmake`. No tests, only build logic. |
| `core/` | Core library unit tests: `ra8_core`, memory arenas, ring buffers, math, timestamping, error reporting, and task scheduler tests. |
| `fixtures/` | Committed input data, grouped by kind: `webp/` decode bitstreams (also the fuzz seed corpus), `epub/` a real-EPUB probe harness, `ra8_fs/` a gzipped exFAT image, and the unzipped EPUB source trees the rabook parity tests read. |
| `fuzz/` | libFuzzer harnesses under `src/`, one `fuzz_ra8_*.c` per target (contract in `fuzz/inc/fuzz_entry.h`). Built only under `RA8_FUZZ`; the registry is `RA8_FUZZ_TARGETS`, and `scripts/checks/run_fuzz.sh` drives a run. |
| `golden/` | Pinned reference renders. Currently gzipped PPM framebuffers of the `ereader_ui` example, produced deterministically by `tools/ra8_emulator` and compared by `just apps::emulator::golden`. |
| `graphics/` | Graphics subsystem and display pipeline unit tests: GLCDC display controller, DRW2D vector accelerator, E-Ink refresh, and pixel format transforms. |
| `hal/` | Hardware Abstraction Layer peripheral tests: GPIO, Timer, RTC, WDT, SPI, I2C, SCI, DMAC, DTC, CAC, and POEG. |
| `host/` | Tests that deliberately link a narrow source subset -- `host/src/exfat_fs_test.c` links only `ra8_fs_fat`, so it builds on macOS as well as Linux. Registered by `tests/cmake/tests_storage.cmake`, not the glob. |
| `misc/` | Miscellaneous subsystem and helper unit tests. |
| `mocks/` | The hardware fakes linked in place of real peripherals under `RA8_OFF_TARGET`: a programmable MMIO fault seam, fake mmap'd register RAM, and fake DMA, IRQ, time, world, XSPI flash and LevelX NOR backends. |
| `net/` | Network stack and protocol tests: Ethernet MAC, lwIP/NetX integrations, socket layers, and transport protocols. |
| `security/` | Cryptography and security tests: RSIP engine, TRNG entropy, TrustZone NSC veneers, and memory protection. |
| `storage/` | File system and storage driver tests: `ra8_fs` FAT12/16/32/exFAT, SDHI controller, LevelX NOR, and XSPI flash memory. |
| `support/` | Shared test utilities: reusable implementations in `src/` and their authored contracts/fixtures in `inc/`. These are linked explicitly where needed and are not discovered as standalone tests. |
| `usb/` | USB controller and class tests: USBFS/USBHS controllers, device mode, host stack, CDC-ACM, MSC, and HID classes. |
| `wireless/` | Wireless and coprocessor interface tests: ESP32-C6 link protocol, Wi-Fi command framing, and BLE transport. |
