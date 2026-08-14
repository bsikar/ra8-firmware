# Media Download Example

This application is hardware-pending. It does not currently open the board's
C6 transport, mount storage, or provide an RSIP/PSA SHA-256 adapter, so its
`main` reports that state and deliberately sends no request. It is not emulator
or HIL evidence.

The reusable path is `ra8_c6link_mdl_transfer()`. A real board application must:

1. open and prove liveness of one `ra8_c6link_t`;
2. provide transactional storage callbacks whose `begin` creates a private
   temporary object, `abort` removes it, and `commit` atomically publishes it;
3. provide `storage.validate` for a `.rabook` destination so its magic, header,
   and bounded structure are checked after SHA equality but before `commit`;
4. provide a caller-owned streaming SHA-256 context;
5. choose finite `chunk_bytes` and `max_chunks` bounds; and
6. call the coordinator with the HTTPS URL and RA8-local destination.

The coordinator pulls only the raw HTTPS response body. It does not scrape a
site, convert formats, or run the host `media_dl` CLI/export pipeline. Content
import remains a separate RA8 application responsibility. A null validator is
an explicit format-agnostic raw transfer; SHA equality alone must not be cited
as proof that the received bytes are a valid RABOOK artifact.

Model verification lives in `tests/test_ra8_c6link.c` and covers successful
multi-chunk commit, short writes, storage exhaustion/removal, digest mismatch,
out-of-order responses, and cancellation. Move this example out of
`hw_pending` only after real adapters are wired and a mixed-image HIL run shows
byte-for-byte equality with no temporary object after injected failure.
