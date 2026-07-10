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
| `fuzz_ra_ble_att`   | ATT dispatcher via `ra_ble_host_test_inject_acl()`      | BLE peer (over the air)      |
| `fuzz_ra_usb_pal`   | `ra_usb_pal_ep_open` / `ep_send` / `ep_recv`            | USB host / compliance stand  |
| `fuzz_ra_tls`       | `ra_tls_*` facade lifecycle + BIO recv stream           | Network transport (TLS)      |
| `fuzz_ra_canfd`     | RX frame parser via `ra_canfd_test_inject_frame()`      | CAN-FD bus                   |
| `fuzz_ra_etha`      | Ethernet header parser via `ra_etha_test_inject_rx()`   | Ethernet                     |
| `fuzz_ra_fs_fat`    | FAT BPB / directory entry walk via `ra_fs_mount()`      | Removable media              |
| `fuzz_ra_jpeg_sw_block` | Focused JPEG Huffman block decoder (dec_block path) | Camera frames                |
| `fuzz_ra_stb_image` | `stbi_load_from_memory()` (PNG/JPEG/GIF/BMP, stb_image) | EPUB cover / figure images   |
| `fuzz_ra_reflow_xml`| `ra_epub_xml_parse_opf/ncx/nav()` (tinyxml2 XML parse)  | EPUB manifest / TOC (XML)    |
| `fuzz_ra_stbtt`     | `stbtt_InitFont()` + glyph raster (stb_truetype font)   | EPUB embedded fonts          |

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

    docker run --rm -v "$PWD:/work" -w /work ra8-firmware-test:latest \
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

## Seed corpora

Each harness ships with a small set of known-good inputs under
`tests/fuzz/corpus/<target>/`. Starting from real coverage rather
than random bytes lets libFuzzer reach interesting parser states in
seconds rather than minutes, which is the difference between the
30-second smoke run finding a regression and missing it.

The seeds are (re-)materialised by `scripts/utils/init_fuzz_corpora.sh`,
which is invoked automatically by `make fuzz` and by
`scripts/utils/run_fuzz.sh` before each session. The script is
idempotent: it overwrites the seed files in place but does not touch
crash reproducers added by the fuzzer or by hand.

| Target              | Seeds | Generation                                                          |
|---------------------|-------|---------------------------------------------------------------------|
| `fuzz_ra_jpeg_sw`   | 5     | `scripts/utils/gen_jpeg_fixture.py` at five (W,H) sizes             |
| `fuzz_ra_epub`      | 2     | Hand-crafted minimal EPUB ZIPs via Python `zipfile`                 |
| `fuzz_ra_modem_at`  | 10    | Plain-text AT response strings (`OK`, `+CSQ:`, `+CME ERROR:`, ...)  |
| `fuzz_ra_ble_att`   | 4     | Hand-built ATT PDUs (FIND_INFO, READ_BY_TYPE, READ, WRITE)          |
| `fuzz_ra_usb_pal`   | 4     | Endpoint-descriptor + payload packets (bulk in/out, intr, iso)      |
| `fuzz_ra_tls`       | 4     | TLS record headers (ClientHello, Alert close, AppData, Finished)    |
| `fuzz_ra_canfd`     | 5     | Raw `CFDRF[0]` frame blobs (classic, extended, FD, min, max DLC)    |
| `fuzz_ra_etha`      | 5     | Short Ethernet frames (ARP, IPv4, VLAN, runt, min header)           |
| `fuzz_ra_fs_fat`    | 4     | Sparse FAT BPB seeds (FAT16 basic / 4 KiB cluster / minimal / zero) |
| `fuzz_ra_jpeg_sw_block` | 4 | Scan-data fragments appended to a fixed JFIF header by the harness  |
| `fuzz_ra_stb_image` | 2     | A minimal 1x1 BMP (valid) plus a truncated/garbage header (malformed) |
| `fuzz_ra_reflow_xml`| 2     | A minimal valid OPF package plus a malformed XML fragment           |
| `fuzz_ra_stbtt`     | 2     | The bundled `libs/fonts/literata_latin1.ttf` plus a garbage blob    |

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

These parsers each consume a byte stream that originates outside the
device's trust boundary (network frames, modem responses, filesystem
media, and -- most CVE-dense of all -- the memory-unsafe SOUP decoders
that ingest a fully attacker-controlled EPUB: `miniz` (ZIP), `stb_image`
(PNG/JPEG/GIF/BMP), `stb_truetype` (embedded fonts), and `tinyxml2` (the
OPF/NCX/nav manifests)). They are also the parsers with the largest and
most state-rich grammar in the codebase, so they are the highest-value
targets per CPU-second of fuzzing. `fuzz_ra_stb_image`, `fuzz_ra_stbtt`,
and `fuzz_ra_reflow_xml` reach the stb / tinyxml2 SOUP directly on every
input, whereas `fuzz_ra_epub` only reaches the XML layer after miniz has
inflated a well-formed ZIP -- so the three focused harnesses give the
parsers far more coverage per second. Smaller parsers (e.g. UART command
shells, internal config blobs) are not yet wrapped because the input is
either trusted or the grammar is small enough that exhaustive
hand-written tests already cover it.
