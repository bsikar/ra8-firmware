# Media Download -- C6 HTTPS to strict SD `.rabook`

This hardware-pending app is the real RA8 composition root for the reusable
media-transfer path. It uses Pmod1 for the ESP32-C6 and Pmod2 for a micro-SD
card containing an existing FAT volume. One run:

1. opens the ESP-hosted C6 link and joins the configured Wi-Fi station;
2. mounts the existing SD volume as `sd` without formatting it;
3. creates `sd:/BOOKS` when absent and refuses a non-directory collision;
4. streams the configured HTTPS response body in 1024-byte RPC pulls;
5. persists each accepted byte to `C6STAGE.TMP` while independently hashing it
   with the caller-owned RA8 SHA-256 stream;
6. compares the C6 terminal digest, closes the stage, strictly validates every
   RBKC zlib stream and the complete inner RABOOK1 structure/CRC;
7. publishes `sd:/BOOKS/C6BOOK.RBK` through the VFS no-replace rename; and
8. reopens, strictly revalidates, reads metadata, and demand-loads the first
   exact inflated chunk from the committed file.

The transfer is bounded to 32 MiB of compressed response data. The RBKC reader
accepts 64 KiB inflated chunks and a table of up to 2048 chunks (128 MiB
inflated). Its table, compressed staging, inflated chunk, and strict-validation
scratch live in external SDRAM. First-party code uses no heap or stdio; miniz is
built with `MINIZ_NO_MALLOC` and `MINIZ_NO_STDIO`.

## Build

Credentials and URL are build inputs, never committed:

```sh
RA8_C6_WIFI_SSID=ra8-bench \
RA8_C6_WIFI_PSK='...' \
RA8_MEDIA_DOWNLOAD_URL='https://host/path/book.rabook' \
make
```

The Makefile also sources the gitignored `coprocessor/esp32c6/wifi.env` when it
exists. An aggregate/no-secret build leaves all three values empty; that image
builds but fails before mounting storage or issuing an RPC.

Publication is deliberately create-new. A second successful run with the same
destination refuses to replace `C6BOOK.RBK`; remove or rename the existing book
through an authorized UI/maintenance path first. Native FAT/exFAT does not
claim power-loss-atomic rename or a durable barrier, and this app does not
invent those guarantees.

## Verification boundary

The cross-build proves the real board, C6, SD/FAT/VFS, strict reader, and SHA
types compose with fixed storage. Host tests exercise success, transport
corruption, coherent invalid RBKC, no-stage cleanup, and exact readback. This
app remains under `hw_pending` until the matching ESP32-C6 image is built with
the pinned ESP-IDF toolchain and physical mixed-image HIL proves success plus
failure cleanup on the EK-RA8D2. No flash is part of the normal build.
