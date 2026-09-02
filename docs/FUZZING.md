# Fuzzing

This project ships [libFuzzer](https://llvm.org/docs/LibFuzzer.html)
(coverage-guided in-process fuzzer; FOSS, ships with `clang`) harnesses
for the parsers most exposed to untrusted input. The harnesses live in
`tests/fuzz/` and are opt-in via the CMake option `RA8_FUZZ=ON`. The
default host test build (and the `just quality::local::test` / `ctest` CI gate) is not
affected.

## Targets

The harnesses cover the parsers that can be fed bytes from outside the
device: the network and cellular-modem byte streams, USB, CAN-FD and
Ethernet frames, the filesystem on removable media, and -- most
CVE-dense of all -- the SOUP decoders that ingest a fully
attacker-controlled book. That last group is the bulk of them: the ZIP
container and the XML inside it, the image codecs, the font rasteriser,
the comic-archive unwrappers, and the shared decompression-limits seam
every decoder passes through.

Add a new harness by dropping `tests/fuzz/src/fuzz_ra8_<x>.c` next to the
existing files and listing it in `tests/fuzz/CMakeLists.txt`
(`RA8_FUZZ_TARGETS`). That registry is the single source of truth:
`scripts/checks/run_fuzz.sh --list` parses it, cross-checks it against
the `tests/fuzz/src/fuzz_ra8_*.c` sources (drift in either direction is a
hard error), and both `just quality::local::fuzz` and the nightly CI sweep consume it
through that script -- there is no second list to update.

## Running

`just quality::local::fuzz` is the smoke run. It delegates to
`scripts/checks/run_fuzz.sh`, which configures and builds
`tests/build-fuzz/` and then gives every harness a short budget; the
build is skipped when no source changed, so a re-run only re-fuzzes.
The same script takes a single target name and a longer budget for a
real session, and it reuses that build tree. Its `--help` is the
authority on the arguments and on the environment variables that move
the wall-clock and iteration caps.

Sweeping with `--all` is not a shell loop over single-target runs: it
keeps going after a crashing target, so one crash cannot mask another,
then exits non-zero listing every harness that failed. Crash inputs
land in `tests/build-fuzz/crashes/<target>/`.

`.github/workflows/fuzz-nightly.yml` is a thin driver for the
`fuzz-sweep` gate (`just quality::gate::run fuzz-sweep`), which runs
that same sweep nightly at a far larger per-target budget. The job
fails on any crash and uploads the crash directory and the full sweep
log as artifacts.

## Toolchain requirements

- `clang` with libFuzzer. gcc has no `-fsanitize=fuzzer` support, and
  the build refuses to configure if the active C compiler is not clang.
  `run_fuzz.sh` auto-selects the first clang on PATH that can link
  `-fsanitize=fuzzer` (bare `clang` first, then versioned majors
  newest-first); `CC` / `CXX` pin one explicitly.
- AddressSanitizer + UndefinedBehaviorSanitizer runtimes (shipped with
  clang automatically; no separate install).

### macOS note

The host test fake (`tests/mocks/src/ra8_fake_mmap.c`) installs RAM at
the same MCU peripheral addresses via `mmap(MAP_FIXED, 0x40000000)`.
macOS arm64 refuses MAP_FIXED below 4 GiB, so all host tests --
including these fuzz harnesses -- run inside the project's Linux
devcontainer. Use `just tests::devcontainer all` for host tests and
`just quality::devcontainer::fuzz` for the fuzz smoke run; the legacy
`scripts/ci/test-docker.sh` test spelling remains a thin delegate. The fuzz CMake file drops
AddressSanitizer when configured on macOS so the build still succeeds
for development, but a real fuzz session belongs in the container.

## Build details

`tests/fuzz/CMakeLists.txt` builds each harness as

    add_executable(fuzz_<x> fuzz_<x>.c $<TARGET_OBJECTS:ra8_core_hal>)

and applies `-fsanitize=fuzzer,address,undefined` only on the harness
translation unit. The reused `ra8_core_hal` OBJECT library is compiled
without those sanitizers (it is shared with the rest of the host test
build and we deliberately do not perturb that). ASan / UBSan still
diagnose out-of-bounds reads and integer UB inside the linked-in
production code; libFuzzer coverage feedback is limited to the harness
file itself.

Extending coverage feedback into the parsers themselves would take a
second OBJECT library recompiled with
`-fsanitize=fuzzer-no-link,address,undefined`. The single-library shape
is a deliberate trade: the harness-only feedback already finds bugs, and
the cmake graph stays simple.

## Seed corpora

Each harness ships with a small set of known-good inputs under
`tests/fuzz/corpus/<target>/`. Starting from real coverage rather
than random bytes lets libFuzzer reach interesting parser states in
seconds rather than minutes, which is the difference between the
smoke run finding a regression and missing it.

The seeds are (re-)materialised by `scripts/builders/init_fuzz_corpora.sh`,
which is invoked automatically by `just quality::local::fuzz` and by
`scripts/checks/run_fuzz.sh` before each session. The script is
idempotent: it overwrites the seed files in place but does not touch
crash reproducers added by the fuzzer or by hand.

A few targets get no generated seed at all -- their input is a
struct-shaped API sequence rather than a file format, so a seed file
buys nothing. The rest get a handful each: a minimal valid input for
every shape the parser branches on, plus a truncated or hostile one.
`init_fuzz_corpora.sh` is the authority on which target gets what.

The corpus directory is passed to libFuzzer as a positional argument.
libFuzzer also writes any *new* coverage-expanding inputs back into
the same directory across runs -- this is how the corpus grows
organically as the parsers gain new branches. Crash inputs are kept
separate (under `tests/build-fuzz/crashes/<target>/`) and should be
hand-promoted into `tests/fuzz/corpus/<target>/` as regression seeds
once the underlying bug is fixed -- see "Filing crashes" below.

## Filing crashes

A crash surfaces as a non-zero exit from the harness, plus a binary
reproducer file in `tests/build-fuzz/crashes/<target>/crash-<sha1>`.
For each crash:

1. Confirm the reproducer with
   `tests/build-fuzz/fuzz/<target> tests/build-fuzz/crashes/<target>/crash-<sha1>`.
2. Add a regression test under `tests/<category>/src/test_<module>.c` that loads the
   reproducer (or a hand-minimised version) and asserts the parser
   returns an error code instead of crashing.
3. Fix the parser. The host suite plus the new regression test must
   pass before the commit lands.
4. Keep the reproducer in version control under
   `tests/fuzz/corpus/<target>/` (create the directory on demand) so
   future fuzz runs replay it as part of the seed corpus.

## Why these targets

These parsers each consume a byte stream that originates outside the
device's trust boundary (network frames, modem responses, filesystem
media, and -- most CVE-dense of all -- the memory-unsafe SOUP decoders
that ingest a fully attacker-controlled EPUB: `miniz` (ZIP), `stb_image`
(PNG/JPEG/GIF/BMP), `stb_truetype` (embedded fonts), and the bounded pure-C
XML reader plus OPF/NCX/nav consumers. They are also the parsers with the largest and
most state-rich grammar in the codebase, so they are the highest-value
targets per CPU-second of fuzzing. `fuzz_ra8_stb_image`, `fuzz_ra8_stbtt`,
and `fuzz_reflow_xml` reach the stb or XML parser directly on every
input, whereas `fuzz_epub` reaches the XML layer only after miniz has
inflated a well-formed ZIP -- so the three focused harnesses give the
parsers far more coverage per second. Smaller parsers (e.g. UART command
shells, internal config blobs) are not yet wrapped because the input is
either trusted or the grammar is small enough that exhaustive
hand-written tests already cover it.
