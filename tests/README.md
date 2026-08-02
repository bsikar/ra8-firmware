# tests/

Host-side test tree. Everything here is compiled with the **host** toolchain
(x86_64 gcc/clang), never cross-compiled -- the target build lives in the
top-level `CMakeLists.txt`. The build is assembled by `tests/CMakeLists.txt`,
which by default registers **one executable per `tests/test_*.c`** (see
`cmake/unit_tests.cmake`); a test that needs a special link, a device gate, or
C++ is instead defined in one of the `cmake/tests_*.cmake` fragments and
removed from that glob.

The loose files that sit directly in `tests/` are the two things that are not a
subdirectory's job: the `test_*.c` / `test_*.cpp` suites themselves, and a
handful of per-test baked-data headers (`comic_fixture.h`, `eth_frame_fixture.h`,
`fixture_ahem.h`, the `rabook_*_fixture.h` set, `unarch_xz_fixture.h`) plus the
`unity_minimal.h` assertion shim. Anything shared across many suites, or that is
input data, a hardware fake, or build machinery, lives in a subdirectory below.

## Subdirectories

Each row's first cell names one immediate subdirectory of `tests/`. The
`tests-readme` CI gate (`scripts/checks/check_tests_readme.py`) fails if this
list drifts from what actually exists -- a new subdirectory that is not
described here, or a row here whose subdirectory is gone.

| Subdirectory | What it holds |
|---|---|
| `bench/` | Host microbenchmarks, not correctness tests. A hand-written harness (`ra8_bench.h`, since Google Benchmark is not vendored) plus one `bench_ra8_*.c` executable per subject (CRC, `gfx_text`, software JPEG); each prints a CSV `name,iterations,ns_per_op,MB_per_s` row. Built via `add_subdirectory(bench)`. |
| `cmake/` | The CMake fragments that assemble the host unit-test build, included **in order** by `tests/CMakeLists.txt`: `host_config` (C/C++ standards, warning profile, the coverage / MC/DC / sanitizer instrumentation modes), `unit_tests` (the `test_*.c` auto-glob and `ra8_add_test()`), `library_sources`, `core_hal`, and the themed `tests_*.cmake` (xml, crypto, storage, npu, ra8_emulator). Holds no tests itself -- only build logic. |
| `fixtures/` | Committed input data consumed by the tests, grouped by kind: `webp/` decode bitstreams (also the `fuzz_ra8_webp` seed corpus), `epub/` a real-EPUB probe harness (real books under `epub/real/` are git-ignored), `ra8_fs/` a gzipped exFAT disk image, and `rabook_parity/` `rabook_realbook/` `rabook_fixed_layout/` unzipped EPUB source trees for the rabook/EPUB parity tests. |
| `fuzz/` | libFuzzer harnesses, one `fuzz_ra8_*.c` per target, each defining `LLVMFuzzerTestOneInput` (the contract is in `fuzz_entry.h`). Built only under `RA8_FUZZ` via `add_subdirectory(fuzz)`; the target registry is `RA8_FUZZ_TARGETS` in `fuzz/CMakeLists.txt`, and `scripts/checks/run_fuzz.sh` drives a run. |
| `golden/` | Pinned reference-render images used as regression goldens. Currently `ereader_chrome/`: gzipped PPM framebuffers of the `ereader_ui` example, rendered deterministically by `tools/ra8_emulator` and compared by `make ereader-golden`. |
| `host/` | Standalone host tests that deliberately link only a narrow source subset -- currently `exfat_fs_test.c`, which links only `ra8_fs_fat` (no `ra8_core_hal`) so it builds and runs on macOS and Linux, unlike the Unity ctest suite. Registered by `cmake/tests_storage.cmake`, not the `test_*.c` glob. |
| `mocks/` | The hardware fakes the host tests link in place of real peripherals under `RA8_OFF_TARGET` (the Dependency-Inversion substitutes): a programmable MMIO fault seam (`ra8_fake_mmio`), fake mmap'd register RAM (`ra8_fake_mmap`), and fake DMA, IRQ, time, world, XSPI flash and LevelX NOR backends. |
| `support/` | Header-only shared test utilities and fixtures (`*_test_util.h`, plus a few `*_fixture.h`): the constants, config builders and helper routines factored out of a `test_*.c` suite when it was split across several binaries. All definitions are `static` so each test binary carries its own copy and the `test_*.c` auto-glob stays free of non-test `.c` files. |
