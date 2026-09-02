# epub_parse

The first firmware app to run the `epub` parse stack on the target (#139).
It bakes a known-good two-chapter `.epub` into a C array and parses it entirely
in memory -- no card, no USB, no display -- driving the ZIP central-directory
read, the bounded XML reader over `META-INF/container.xml` and the OPF, the
miniz DEFLATE decompressor on chapter 0, and the Dublin Core metadata read.

The verdict includes a CRC-32 over chapter 0's decompressed XHTML, which is
byte-exact for the baked fixture, so a pass proves the bytes are right rather
than that nothing crashed.

## Zero-heap is the point

This firmware traps `_sbrk` (NASA Rule 3), so every allocation has to be
accounted for: miniz allocates through the `epub_miniz_alloc` static
first-fit arena, and XML parsing uses caller-owned workspace and allocates
nothing. A stray `malloc` anywhere in the parse path reaches the trap and the
board hangs before the banner -- which is the most useful failure this app
reports.

miniz's ZIP central-directory math leans on the Armv8.1-M 64-bit long-shift
family (`LSLL` / `LSRL` / `ASRL`). Anything emulating the M85 on a plain M33
core has to implement those explicitly; without them the ZIP parse fails with
miniz `INVALID_HEADER_OR_CORRUPTED` rather than anything that points at the real
cause.

Needs no card and no external hardware. Large and adversarial archives are the
host fuzzer's job.
