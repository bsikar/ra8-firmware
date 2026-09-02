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

## Runtime configuration

Every build is credential-free. After boot the image prints
`ra8_net_provision: READY v1` and accepts exactly one bounded packet over the
debug UART:

```text
RA8NET1:<ssid_hex>:<psk_hex>:<url_hex>
```

Unlike the join and camera examples, this app requires the URL field. Invalid,
missing, or oversized input fails before storage publication or a media RPC.
Received bytes are never echoed. Wi-Fi fields are erased after association and
the URL is erased after the transfer returns, so neither compiler metadata nor
firmware artifacts contain runtime configuration.

## Blocked on

Physical mixed-image HIL. The configured cross-build proves the board, C6, SD /
FAT / VFS, strict reader, source formatter and SHA types compose with fixed
storage, and the matching ESP32-C6 image compiles and links from current sources
with post-link checks for the strong media handler and the component ABI marker.
Host tests exercise success, transport corruption, coherent-but-invalid RBKC,
no-stage cleanup and exact readback. What is missing is a run on the board proving
success **and** failure cleanup.
