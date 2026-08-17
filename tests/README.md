# tests/

Host-side tests, compiled with the host toolchain and never cross-compiled --
the target build is the top-level `CMakeLists.txt`. `make test` runs them.

`tests/CMakeLists.txt` registers **one executable per `tests/test_*.c`**. A test
needing a special link, a device gate, or C++ is instead defined in one of the
`cmake/tests_*.cmake` fragments and removed from that glob.

Only two things sit loose in `tests/`: the `test_*.c` / `test_*.cpp` suites, and
the per-test baked-data headers beside them (`comic_fixture.h`,
`eth_frame_fixture.h`, `fixture_ahem.h`, the `rabook_*_fixture.h` set,
`unarch_xz_fixture.h`) plus the `unity_minimal.h` assertion shim. Anything
shared across suites, or that is input data, a hardware fake, or build
machinery, belongs in a subdirectory.

## Subdirectories

The `tests-readme` gate (`scripts/checks/check_tests_readme.py`) reads the first
cell of each row below and fails if it drifts from the tree in either direction
-- an undescribed subdirectory, or a row whose subdirectory is gone.

| Subdirectory | What it holds |
|---|---|
| `bench/` | Host microbenchmarks, not correctness tests: one `bench_ra8_*.c` per subject over a hand-written harness (`ra8_bench.h`), each printing a CSV `name,iterations,ns_per_op,MB_per_s` row. |
| `cmake/` | The fragments that assemble the host build, included in order: `host_config` (standards, warnings, coverage / MC/DC / sanitizer modes), `unit_tests` (the auto-glob and `ra8_add_test()`), `library_sources`, `core_hal`, then the themed `tests_*.cmake`. No tests, only build logic. |
| `fixtures/` | Committed input data, grouped by kind: `webp/` decode bitstreams (also the fuzz seed corpus), `epub/` a real-EPUB probe harness, `ra8_fs/` a gzipped exFAT image, and the unzipped EPUB source trees the rabook parity tests read. |
| `fuzz/` | libFuzzer harnesses, one `fuzz_ra8_*.c` per target (contract in `fuzz_entry.h`). Built only under `RA8_FUZZ`; the registry is `RA8_FUZZ_TARGETS`, and `scripts/checks/run_fuzz.sh` drives a run. |
| `golden/` | Pinned reference renders. Currently gzipped PPM framebuffers of the `ereader_ui` example, produced deterministically by `tools/ra8_emulator` and compared by `make ereader-golden`. |
| `host/` | Tests that deliberately link a narrow source subset -- `exfat_fs_test.c` links only `ra8_fs_fat`, so it builds on macOS as well as Linux. Registered by `cmake/tests_storage.cmake`, not the glob. |
| `mocks/` | The hardware fakes linked in place of real peripherals under `RA8_OFF_TARGET`: a programmable MMIO fault seam, fake mmap'd register RAM, and fake DMA, IRQ, time, world, XSPI flash and LevelX NOR backends. |
| `support/` | Header-only shared utilities and fixtures factored out when a suite was split across binaries. All definitions are `static`, so each binary carries its own copy and the `test_*.c` glob stays free of non-test `.c` files. |
