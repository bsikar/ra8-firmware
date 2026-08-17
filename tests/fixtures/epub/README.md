# Real-world EPUB test fixtures + probe

Drop real `.epub` files into `real/` and probe them against the `ra8_epub`
pipeline (parse / spine / TOC / cover / chapter-inflate) on the host. This is
how real-world EPUB compatibility gaps get caught -- the synthetic baked
fixtures under `tests/` only exercise what someone already thought to write.

## Copyright

`real/*.epub` is **git-ignored** -- the books are copyrighted and used for
local testing only; they are never committed or published. Copy your own EPUBs
in. If a redistributable fixture is ever needed in the repo, use a public-domain
EPUB (Project Gutenberg, say), not a copyrighted book.

## Probe

`epub_probe.c` opens an `.epub` through `ra8_epub_open()` (host /
`RA8_OFF_TARGET`, malloc-backed) and prints what the pipeline extracts;
`run_probe.sh` beside it builds and runs it against a book you name.

Probing real sideloaded books is what surfaced the pipeline's static-buffer
ceilings: on device there is no heap, so every EPUB the reader can open is
bounded by fixed arenas -- the miniz pool in `ra8_epub_miniz_alloc.h` and the
shared OPF/NCX scratch in `ra8_epub_open.c`. A book that exceeds either fails
`no_mem`, and the failure surfaces as a missing TOC or a book that will not
open at all rather than as anything that names a buffer. When a real book
fails here, check those two sizes first.
