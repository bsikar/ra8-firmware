# media_download

The RA8 composition root for the reusable media-transfer path: an HTTPS source
reached over the ESP32-C6 on Pmod1, landing as a strictly-validated `.rabook` on
a micro-SD card in Pmod2.

One run opens the ESP-hosted C6 link and joins the configured station, mounts the
card's **existing** FAT volume without formatting it, creates `sd:/BOOKS` when
absent (and refuses a non-directory collision), then streams the HTTPS response
body in bounded RPC pulls. The body is either a prebuilt `.rabook` or -- in
source-image mode -- one bounded encoded image which the RA8 decodes, normalizes,
emits as a one-page RABOOK1 book and wraps as an RBKC random-access container.
**That mode is conversion, not relabeling**, and its new bytes go through the same
strict validator as everything else.

The artifact is hashed independently, its private stage is closed, and every RBKC
zlib stream plus the inner RABOOK1 structure is strictly validated **before** the
file is published through the VFS no-replace rename. The app then reopens the
committed file, revalidates it, reads metadata, and demand-loads the first exact
inflated chunk.

The transfer, the inflated chunk size and the chunk-table length are all bounded.
The table, compressed staging, inflated chunk and strict-validation scratch live
in external SDRAM. First-party code uses no heap and no stdio; miniz is built with
`MINIZ_NO_MALLOC` and `MINIZ_NO_STDIO`.

Publication is deliberately create-new: a second successful run with the same
destination **refuses** to replace the existing book, which must be removed
through an authorized maintenance path first. Native FAT/exFAT does not claim a
power-loss-atomic rename or a durable barrier, and this app does not invent those
guarantees.

## Configuration and the fail-closed image

Credentials and the URL are build inputs and are never committed; the Makefile
also sources the gitignored `coprocessor/esp32c6/wifi.env` when it exists. An
aggregate or no-secret build leaves them empty, and that image **builds but fails
before mounting storage or issuing an RPC** -- it is fail-closed by design.

That matters for evidence: an empty-config build proves nothing about whether the
linker retained the feature. `make compile-proof` exists for exactly that gap. It
builds with reserved, non-secret values in a separate directory, keeping the
complete transfer path reachable under optimization, and then asserts that the
final ELF contains the C6 transfer and strict RABOOK validation symbols plus a
nonempty SDRAM workspace.

## Blocked on

Physical mixed-image HIL. The configured cross-build proves the board, C6, SD /
FAT / VFS, strict reader, source formatter and SHA types compose with fixed
storage, and the matching ESP32-C6 image compiles and links from current sources
with post-link checks for the strong media handler and the component ABI marker.
Host tests exercise success, transport corruption, coherent-but-invalid RBKC,
no-stage cleanup and exact readback. What is missing is a run on the board proving
success **and** failure cleanup.
