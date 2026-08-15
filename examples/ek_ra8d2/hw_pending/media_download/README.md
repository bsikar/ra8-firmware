# Media Download Example

This application is hardware-pending. It does not currently open the board's
C6 transport, mount storage, or provide an RSIP/PSA SHA-256 adapter, so its
`main` reports that state and deliberately sends no request. It is not emulator
or HIL evidence.

The reusable path is `ra8_c6link_mdl_transfer()`. A real board application must:

1. open and prove liveness of one `ra8_c6link_t`;
2. provide transactional storage callbacks whose `begin` creates a private
   temporary object, `abort` removes it, and `commit` publishes it according to
   the selected backend's stated capabilities;
3. bind `ra8_mdl_rabook_vfs_validate` to `storage.validate` so every RBKC chunk
   and the complete inner RABOOK1 structure/CRC are checked after SHA equality
   but before `commit`;
4. provide a caller-owned streaming SHA-256 context;
5. choose finite `chunk_bytes` and `max_chunks` bounds; and
6. call the coordinator with the HTTPS URL and RA8-local destination.

The coordinator pulls only the raw HTTPS response body. It does not scrape a
site, convert formats, or run the host `media_dl` CLI/export pipeline. Content
import remains a separate RA8 application responsibility. A null validator is
an explicit format-agnostic raw transfer; SHA equality alone must not be cited
as proof that the received bytes are a valid RABOOK artifact.

Protocol and coordinator verification lives in `tests/test_ra8_c6link.c` and
the related C6-link tests. `tests/test_ra8_c6link_rabook.c` additionally
generates a real `.rabook`, sends it through an independently decoded model C6
RPC, stages and publishes it on real FAT16/VFS storage, strictly reopens and
consumes it, and checks transport-corruption and invalid-format cleanup.

That host mixed-image test does not close the board gap. Move this example out
of `hw_pending` only after the EK-RA8D2 composition has a proven C6 transport,
a selected and mounted SD/XSPI backend with enough workspace, an arbitrary-
length streaming SHA-256 adapter, and a mixed-image physical HIL run showing
byte-for-byte equality with no temporary object after injected failure. The C6
image itself must be built with the pinned ESP-IDF toolchain on the Pi bench
host; the dev host intentionally has no `idf.py` installation.
