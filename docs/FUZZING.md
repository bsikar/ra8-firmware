# Fuzzing

This project ships [libFuzzer](https://llvm.org/docs/LibFuzzer.html)
(coverage-guided in-process fuzzer; FOSS, ships with `clang`) harnesses
for the parsers most exposed to untrusted input. The harnesses live in
`tests/fuzz/` and are opt-in via the CMake option `RA_FUZZ=ON`. The
default host test build (and the `make test` / `ctest` CI gate) is not
affected.

## Targets

The first wave covers every parser that may be fed bytes from outside
the device (network, modem, removable media):

| Target              | Code under test                                         | Trust boundary               |
|---------------------|---------------------------------------------------------|------------------------------|
| `fuzz_ra_jpeg_sw`   | `ra_jpeg_sw_decode()` baseline JPEG decoder             | Camera frames, on-disk files |
| `fuzz_ra_epub`      | `ra_epub_open()` (miniz ZIP + OPF/NCX parsing)          | Removable media              |
| `fuzz_ra_modem_at`  | AT response parser (`ra_modem_at_send_cmd` + rx pump)   | Cellular modem byte stream   |
| `fuzz_ra_net_arp`   | `ra_net_arp_handle()` via `ra_net_test_inject_frame()`  | Ethernet                     |
| `fuzz_ra_net_ipv4`  | IPv4 dispatch (ICMP / UDP / TCP) via the same entry     | Ethernet                     |

Add a new harness by dropping `tests/fuzz/fuzz_<x>.c` next to the
existing files, listing it in `tests/fuzz/CMakeLists.txt`
(`RA_FUZZ_TARGETS`), and (if you want it in the smoke run) adding it to
`FUZZ_TARGETS` in the top-level `Makefile`.

## Running

### Smoke run (default ~30 seconds per target)

    make fuzz

This configures `tests/build-fuzz/` with `-DRA_FUZZ=ON`, builds every
harness, then runs each one with `-max_total_time=30 -runs=10000`. The
target build is skipped when no source changed; subsequent invocations
only re-run the harnesses.

Override the budget per harness:

    FUZZ_SECONDS=120 make fuzz

### Long-form session on one target

    bash scripts/utils/run_fuzz.sh fuzz_ra_jpeg_sw 600

The script reuses the same `tests/build-fuzz/` tree and writes any
crash inputs to `tests/build-fuzz/crashes/<target>/`.

## Toolchain requirements

- `clang` (any version with libFuzzer; the project is verified with
  `clang-18` on Linux). gcc has no `-fsanitize=fuzzer` support and the
  build will refuse to configure if the active C compiler is not
  clang.
- AddressSanitizer + UndefinedBehaviorSanitizer runtimes (shipped with
  clang automatically; no separate install).

### macOS note

The host test simulator (`tests/mocks/ra_sim_mmap.c`) installs RAM at
the same MCU peripheral addresses via `mmap(MAP_FIXED, 0x40000000)`.
macOS arm64 refuses MAP_FIXED below 4 GiB, so all host tests --
including these fuzz harnesses -- run inside the project's existing
Ubuntu 24.04 devcontainer (`scripts/test-docker.sh`). The fuzz CMake
file drops AddressSanitizer when configured on macOS so the build
still succeeds for development, but for a real fuzz session use the
Linux container:

    docker run --rm -v "$PWD:/work" -w /work ra8d2-firmware-test:latest \
        make fuzz

## Build details

`tests/fuzz/CMakeLists.txt` builds each harness as

    add_executable(fuzz_<x> fuzz_<x>.c $<TARGET_OBJECTS:ra_core_hal>)

and applies `-fsanitize=fuzzer,address,undefined` only on the harness
translation unit. The reused `ra_core_hal` OBJECT library is compiled
without those sanitizers (it is shared with the rest of the host test
build and we deliberately do not perturb that). ASan / UBSan still
diagnose out-of-bounds reads and integer UB inside the linked-in
production code; libFuzzer coverage feedback is limited to the harness
file itself.

Future work: spin up a second OBJECT library that recompiles the same
sources with `-fsanitize=fuzzer-no-link,address,undefined` so coverage
feedback reaches the parsers themselves. Not done yet because the
current approach already finds bugs and keeps the cmake graph simple.

## Filing crashes

A crash surfaces as a non-zero exit from the harness, plus a binary
reproducer file in `tests/build-fuzz/crashes/<target>/crash-<sha1>`.
For each crash:

1. Confirm the reproducer with
   `tests/build-fuzz/<target> tests/build-fuzz/crashes/<target>/crash-<sha1>`.
2. Add a regression test under `tests/test_<module>.c` that loads the
   reproducer (or a hand-minimised version) and asserts the parser
   returns an error code instead of crashing.
3. Fix the parser. The existing 190-test host suite plus the
   regression test must pass before the commit lands.
4. Keep the reproducer in version control under
   `tests/fuzz/corpus/<target>/` (create the directory on demand) so
   future fuzz runs replay it as part of the seed corpus.

## Why these targets

These five parsers each consume a byte stream that originates outside
the device's trust boundary (network frames, modem responses,
filesystem media). They are also the parsers with the largest and most
state-rich grammar in the codebase, so they are the highest-value
targets per CPU-second of fuzzing. Smaller parsers (e.g. UART command
shells, internal config blobs) are not yet wrapped because the input
is either trusted or the grammar is small enough that exhaustive
hand-written tests already cover it.
