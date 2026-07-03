# rsip_sha256_kat

On-silicon FIPS 180-4 known-answer test for the RA8D2 Secure IP (RSIP-E50D)
**hardware** SHA-256 engine.

`ra_rot` re-computes an image digest on silicon through `ra_rsip_sha256` (the
RSIP HASH engine), so its correctness is security-critical -- yet no app had
ever driven the RSIP hardware on this board. This app closes that gap: it hashes
three published NIST vectors on the real engine and checks the digests.

Unlike `crypto_aes_demo` (which forces `RA_SIMULATOR_MODE` and therefore runs the
in-tree software fallback), this app links `ra_rsip` **without** the simulator
define, so `ra_rsip_sha256` dispatches to the hardware HASH engine.

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

## Silicon status: FAILS -- this app surfaced a real RSIP-hardware defect

Flashed to the EK-RA8D2 on 2026-07-03, this app is the **first** to drive the
RSIP hardware on the board, and it exposed that the hand-written `ra_rsip`
register-poke driver does **not** work on silicon:

1. `ra_rsip_init(.run_bist = true)` returns an error -- the RSIP BIST self-test
   never passes, so the app is built with `run_bist = false` to get past init.
2. With BIST disabled, `ra_rsip_init` succeeds but `ra_rsip_sha256` returns
   `k_ra_err_hw_timeout` (0x03) with an **all-zero** digest: the poll of
   `HASH_STATUS.READY` / `HASH_STATUS.DONE` in `ra_rsip.c` never sees the bit
   assert (`internal_wait_bit` exhausts `k_ra_rsip_poll_budget`).

Observed UART:

```
kat: boot (console up)
kat: rsip_init ok
rsip sha256: KAT FAIL      (repeating; digest was 0x0000...0000, rc=0x03)
```

Root cause hypothesis: the RSIP-E50D is not a plainly register-mapped peripheral.
Renesas does not fully document its low-level interface in the HUM; FSP drives it
through opaque procedural "primitive" command sequences. `ra_rsip.c` was derived
from that reference but hand-written at the register level and, until this app,
had never run on silicon (the host tests all use `RA_SIMULATOR_MODE`, i.e. the
software fallback, so they cannot catch this).

**Security impact:** `ra_rot`'s on-silicon image digest is computed by
`ra_rsip_sha256`, so a root-of-trust build using the RSIP backend would time out
on silicon. The verified-correct software SHA-256 (tf-psa-crypto, KAT'd by
`tests/test_psa_real_kat.c`) is the path a working on-silicon RoT should use.

This app stays in `hw_pending` as the regression harness: when the RSIP driver is
fixed (or a board/DLM state that permits it is used), it will print
`rsip sha256: KAT OK` and can be promoted to `hw_validated/hil`.
