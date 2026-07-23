# epub_stress

Headless HIL gate proving a **large-structure EPUB opens on the firmware static
arena** (#144 bug 1 regression net).

## Status: demoted to hw_pending (#170 audit)

This app **does not pass on silicon** and lives under `hw_pending/`, not
`hw_validated/hil/`. On the EK-RA8D2 bench (UART reader attached before the
reset, so the #390 print-once race cannot explain it) it prints
`epub-stress-hil: boot` then `epub-stress-hil: FAIL toc`: the NCX navPoint
extraction this gate exists to hold comes back short of 60 on the real part.
The fixture is baked in memory -- no SD card, no external hardware, no
provisioning -- so this is a firmware defect, not a rig gap, and it is tracked.
board_sim cannot arbitrate it either (it stops on an Armv8.1-M encoding the
Unicorn M33 model has no seam for). See `hil.conf` for the full capture.
Re-promote only from a bench capture showing the PASS banner.

## The bug it pins (#144)

A ~7 MB real Boox novel was reported to fail `ra8_epub_open` with
`k_ra8_err_no_mem`. The cause was diagnosed and resolved:

- On the firmware target, miniz's ZIP **central directory** and tinyxml2's OPF
  **DOM** both allocate from the *same* 96 KiB static arena
  (`ra8_epub_miniz_alloc` + the arena-backed `operator new` in
  `ra8_epub_cpp_alloc.cpp`).
- The actual `no_mem` was not the arena -- it was the 16 KiB shared OPF/NCX
  scratch buffer (`k_ra8_epub_opf_xml_buf`) overflowing on a book with a large
  OPF / NCX. That buffer is now 48 KiB (the #144 NCX fix).
- Measured: the 96 KiB shared arena comfortably holds a 125-entry archive (the
  real book has 108 files) -- so the arena itself was never the limit.

## What it does

Opens a baked **synthetic** large-structure EPUB in memory and asserts the
shared arena + parsers handle it:

- 60 chapters (spine, just under the `k_ra8_epub_max_chapters` = 64 cap),
- 60 extra manifest resources + a cover + an NCX with 60 navPoints,
- **125 archive entries / a ~10 KB OPF** -- more files than the 108-file,
  41-chapter real book.

On success it prints:

```
epub-stress-hil: files=125 chapters=60 toc=60 cover=ok PASS
```

asserting `ra8_epub_open` returned `k_ra8_ok` (arena sufficient), all 60 chapters
parsed, all 60 NCX navPoints extracted (#144 bug 2), and the cover-image
manifest item resolved. The fixture is synthetic (not the copyrighted novel),
tens of KB, so it bakes into MRAM and opens in memory like `epub_parse` --
committable and CI-able, unlike the git-ignored real books under
`tests/fixtures/epub/real/`.

## Why a synthetic fixture

The pool pressure during `ra8_epub_open` comes from the file **count** (miniz
central directory) and the OPF item **count** (tinyxml2 DOM), not the total
byte size. A synthetic book with many tiny files reproduces -- and exceeds --
a 7 MB book's shared-arena pressure in tens of KB, so it bakes into MRAM and
stays committable. The `RA8_SIMULATOR_MODE` host build routes miniz + `operator
new` to malloc, so only this on-target (board_sim / silicon) gate exercises the
real static-pool path.

## Validation

Run on `tools/board_sim` (the firmware boots, opens the 125-entry book through
the shared 96 KiB arena, no fault):

```
[uart] SCI8: epub-stress-hil: boot
[uart] SCI8: epub-stress-hil: files=125 chapters=60 toc=60 cover=ok PASS
```

## Regenerating the fixture

```
cd examples/ek_ra8d2/hw_pending/epub_stress
python3 make_stress_fixture.py   # rewrites epub_stress_fixture.h
```

## Build

```
make epub_stress
make -C examples/ek_ra8d2/hw_pending/epub_stress flash
```
