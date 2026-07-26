# rsip_sha256_kat

On-silicon FIPS 180-4 known-answer test for `ra8_rsip_sha256` on the EK-RA8D2.

`ra8_rot` re-computes an image digest on silicon through `ra8_rsip_sha256`, so its
correctness is security-critical. This app hashes three published NIST vectors on
the real M85 and checks the digests, reporting over the board UART.

Vectors (FIPS 180-4):

| input                              | expected SHA-256      |
|------------------------------------|-----------------------|
| `""`                               | `e3b0c442...7852b855` |
| `"abc"`                            | `ba7816bf...f20015ad` |
| 56-byte two-block message          | `248d6a61...19db06c1` |

- **LED1** toggles on every all-vectors-pass cycle; **LED2** latches on failure.
- `rsip sha256: KAT OK` / `rsip sha256: KAT FAIL` is logged over SCI8
  (115200 8N1, J-Link OB CDC). `hil.conf` scrapes it for automated validation.

```
make rsip_sha256_kat                 # cross-compile
make hil-flash APP=rsip_sha256_kat   # flash the Pi-attached board + scrape UART
```

## Silicon status: PASSES (`rsip sha256: KAT OK`)

Flashed to the EK-RA8D2 on 2026-07-03, this app prints `rsip sha256: KAT OK` once
per second -- `ra8_rsip_sha256` produces the correct FIPS 180-4 digests on the
real M85. But getting there is the story:

### The defect it first surfaced

This was the first app ever to drive the RSIP hardware on the board, and it
exposed that the hand-written RSIP register-poke path does **not** work on
silicon:

1. `ra8_rsip_init(.run_bist = true)` returned an error -- the RSIP BIST self-test
   never passed.
2. With BIST disabled, `ra8_rsip_sha256` returned `k_ra8_err_hw_timeout` (0x03)
   with an **all-zero** digest: the poll of `HASH_STATUS.READY` / `DONE` never saw
   the bit assert. A J-Link read of the RSIP register window (`0x403B0000`+) found
   every register reading `0x00000000`, and the driver's writes did not stick.

Root cause: the **RSIP-E50D is not register-mapped**. HUM Ch 52
"Renesas Secure IP (RSIP-E50D)" is a 6-page feature overview (p 3302-3307) with no
register interface; Renesas drives the engine through FSP's opaque procedural
"primitive" command sequences. The register map in `ra8_rsip.c` was invented and
had never run on silicon -- the host tests all use `RA8_SIMULATOR_MODE`, i.e. the
software fallback, so they could not catch it.

### The fix

`ra8_rsip_sha256` now computes the digest with the in-tree software SHA-256
(`RA8_RSIP_SOFTWARE_BACKEND`, enabled unconditionally in `ra8_rsip.c`); the invented
hardware register sequence is retained but never compiled, behind
`RA8_RSIP_HASH_HARDWARE`, as a reference for a future FSP port. `ra8_rot`'s
on-silicon image digest therefore works, and the three former register-plumbing
host tests in `test_ra8_rsip_core.c` are now real SHA-256 known-answer tests.
