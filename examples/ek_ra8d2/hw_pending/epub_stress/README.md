# epub_stress

Opens a baked synthetic large-structure EPUB in memory and asserts that the
firmware's bounded ZIP arena and XML workspaces handle it -- a regression net
for #144. The fixture carries a full spine just under the
`k_ra8_epub_max_chapters` cap, an NCX with a navPoint per chapter, a cover, and
more archive entries than the real book that prompted the bug.

## It does not pass on silicon

On the bench it prints its boot line and then fails on the TOC: the NCX navPoint
extraction this gate exists to hold comes back short on the real part. The UART
reader was attached before the reset, so the #390 print-once race cannot explain
it, and the fixture is baked in memory -- no SD card, no external hardware, no
provisioning -- so this is a firmware defect rather than a rig gap (#170).
`ra8_emulator` cannot arbitrate it either: it stops on an Armv8.1-M encoding the
Unicorn M33 model has no seam for. `hil.conf` holds the capture. Re-promote only
from a bench capture showing the PASS banner.

## The bug it pins (#144)

A large real novel was reported to fail `ra8_epub_open` with
`k_ra8_err_no_mem`. The cause was not the miniz ZIP central-directory arena, which
comfortably holds more entries than that book has; it was the shared OPF/NCX
scratch buffer (`k_ra8_epub_opf_xml_buf`) overflowing on a book with a large OPF
and NCX. That buffer was enlarged. The bounded XML reader itself uses
caller-owned workspace and performs no allocation at all.

## Why a synthetic fixture

ZIP arena pressure comes from the file **count**; XML semantic and scratch bounds
come from the OPF item and NCX entry counts, not total byte size. A synthetic
book of many tiny files and many metadata entries exercises both without shipping
copyrighted content, and it is small enough to bake into MRAM -- committable and
CI-able, unlike the git-ignored real books under `tests/fixtures/epub/real/`.
Regenerate it with `make_stress_fixture.py` beside `main.c`.
