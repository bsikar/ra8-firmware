<!--
SPDX-License-Identifier: MIT
Copyright (c) 2026 Brighton Sikarskie
-->

# secure_app_vault_demo

First consumer of `libs/ra8_secure_app` outside the security tests.

The ereader app compiles the Ring 5 secure sources, but no application in the
tree ever includes `key_vault.h` or `ota_commit.h`. This app calls both.

## What it checks

| Leg      | Check                                                                     |
| -------- | ------------------------------------------------------------------------- |
| `vault`  | Init, store into two slots, challenge digest deterministic per slot.      |
| `vault`  | Two slots holding different keys produce different digests.               |
| `guards` | Out-of-range slots and null pointers refused with the documented codes.   |
| `kak`    | A 16-byte KAK round-trips; a 32-byte KAK of different material replaces   |
|          | it in full, compared over all 32 bytes rather than by length alone.       |
| `kak`    | A 24-byte length, a null key, and an undersized destination are refused.  |
| `ota`    | Reset drops any pending commit; `pending` reports no data when clean.     |
| `ota`    | Arming a bank makes it readable; a second arm while pending is refused.   |
| `ota`    | The bank-config write masks all but the two BANK_SEL bits (off-target).   |

## No option-region writes

The commit and bank-config paths are shadow registers by design: the real
OFS3 / BTFLG write is bench-gated and brick-risky, and on a silicon build
both calls report `k_ra8_err_not_supported`. The `ota` leg accepts either
answer, so it is honest on both build flavours rather than asserting a
behaviour only the off-target build has.

That cuts both ways, and the table above says so: on silicon
`ra8_ota_commit_swap_bank` returns `k_ra8_err_not_supported` and the leg
returns `k_ra8_ok` there, *before* the bank-config write. So the masking row,
and the pending / second-arm rows with it, are proved by the off-target build
only. A bench run of this app confirms the refusals, not the shadow
behaviour.

Nothing here provisions a real device key. The key material is a repeated
fill byte, so the digests prove determinism and slot binding without putting
anything sensitive in the tree.

## Build and run

```sh
cmake --preset ra8d2-debug
ninja -C cmake-build-debug secure_app_vault_demo.elf
```

Flash and watch SCI8 (J-Link OB VCOM, 115200 8N1). A good run prints one
verdict per leg then:

```
secure_app_vault_demo: ALL PASS
```

## Related

- `libs/ra8_secure_app/inc/key_vault.h`
- `libs/ra8_secure_app/inc/ota_commit.h`
- `tests/security/src/test_secure_app_key_import.c`
