# secure_app_vault_kat

Drives the whole `ra8_secure_app` public API (`key_vault.h` + `ota_commit.h`)
once from a real firmware composition root and self-checks every return against
what that function's own header promises. Closes the consumer gap in #922:
before this app, nothing under `apps/` or `examples/` included either header,
so the only callers were `tests/security/`, and nothing proved the API is
usable from an application or caught an ABI or wiring regression the host
fakes paper over.

No production code changes: this is a consumer, not a library edit.

## What it checks

Every step logs `kat: step=N rc=E PASS|FAIL`, where `rc` is the raw `ra8_err_t`
the library returned, so a bench operator can see exactly which entry point
disagreed with its documentation. The verdict line `kat: secure_app PASS`
prints only when all 27 steps matched.

Steps 1-16, `key_vault.h`. The placeholder key vault compiles its real body
only under `RA8_INSECURE_STUB_CRYPTO` (or off-target); its `#else` fails every
entry point closed (#180). This app asserts whichever half it was built
against, so the same source proves both directions:

- Default image, `RA8_INSECURE_STUB_CRYPTO` off: every vault entry point must
  return `k_ra8_err_not_supported`, the caller's digest buffer must come back
  untouched, and `mac_len` must stay 0. This is the first place the fail-closed
  guard is checked from an application rather than a host test.
- Declared dev/eval image, `-DRA8_INSECURE_STUB_CRYPTO=ON`: the challenge path
  is checked against a SHA-256 known-answer vector computed off-target from
  FIPS 180-4 over `key XOR challenge`, independently of the sponge in
  `key_vault.c`. The KAK round-trip and the documented range, NULL and
  key-length error paths are checked too.

The NULL-pointer steps (4, 7, 8, 11) expect `k_ra8_err_null_ptr` in **both**
images: the fail-closed guard must not swallow argument validation.

Steps 17-27, `ota_commit.h`, identical in both images. The option-region writes
are bench-gated, so on silicon `ra8_ota_commit_swap_bank` and
`ra8_ota_commit_set_bank_config` are fail-closed (T5-10) while reset, read-back
and argument validation are live. The read-backs after each refused write are
the point: an out-of-enum bank must be rejected before the fail-closed return,
no swap may look armed afterwards, and the bank-config shadow must still read
0, so a caller can never mistake an unwritten option byte for an armed swap.

## Running it

Bare EK-RA8D2. No expansion board, no external wiring, no fused part, and
nothing here touches key material, signing keys or the option region.

```
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/toolchain-ra8d2.cmake
cmake --build build
# dev/eval variant, the one that runs the SHA-256 KAT:
cmake -S . -B build-stub -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/toolchain-ra8d2.cmake \
      -DRA8_INSECURE_STUB_CRYPTO=ON
cmake --build build-stub
```

`ra8_emulator` does not model the SCI8 console scrape this app reports through,
so the HIL verdict is bench-only: it sits in `hw_pending` until a board run
confirms `kat: secure_app PASS` in both configurations.
