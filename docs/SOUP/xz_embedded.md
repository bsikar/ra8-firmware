# SOUP Justification: XZ Embedded (decode-only)

Per IEC 61508-3 Section 7.4.2.12 and DO-178C Section 12.1.4, this document
records the qualification basis for accepting XZ Embedded into this firmware
as Software Of Unknown Provenance (SOUP).

## Component identity

- **Name**: XZ Embedded (decode-only XZ/LZMA2 implementation)
- **Version**: upstream tag `v2024-12-30`, commit
  `ae63ae3a36ed01724674e8f3d750dc47bf125410` (the upstream project has no
  in-source version macro; the pin was recovered by tree fingerprint --
  all 11 vendored files are byte-identical to that commit, the single exact
  match among the 169 commits reachable from the upstream default branch).
- **Upstream URL**: https://github.com/tukaani-project/xz-embedded
- **Local path**: `libs/third_party/xz_embedded/`
- **Integrity**: two derived checks, neither of them a constant transcribed
  into prose. `docs/sbom/upstream/xz_embedded.manifest` carries the git blob
  SHA-1 upstream publishes for every one of the 11 vendored files, written by
  a real fetch of upstream and compared against this tree by
  `scripts/checks/check_soup_upstream.py` (the `soup-upstream` gate);
  `docs/sbom/ra8-firmware.cdx.json` carries the per-run digest that
  `gen_sbom.py` re-derives from the directory on every run. There is
  deliberately **no** integrity-hash field in `scripts/gen/sbom_registry.py`:
  #538 deleted the stored `aggregate_sha256` literals precisely because a
  hand-transcribed constant compared against itself reported clean on a
  mutated vendored byte.

## Provenance

- **Origin**: Lasse Collin (Tukaani project); major parts based on Igor
  Pavlov's LZMA SDK. The same decoder ships inside the Linux kernel
  (`lib/xz/`).
- **License**: 0BSD (`COPYING`; SPDX `0BSD` headers per file). Zero-clause
  BSD imposes no attribution conditions; the license text is nevertheless
  preserved in-tree and listed in `THIRD_PARTY_LICENSES.md`.
- **How it entered our tree**: flattened vendored subset of upstream
  `linux/lib/xz/` plus `linux/include/linux/xz.h`: the stream decoder
  (`xz_dec_stream.c`), the LZMA2 decoder (`xz_dec_lzma2.c`), the CRC32 /
  CRC64 integrity checkers, their private headers, and the upstream
  `AUTHORS` / `COPYING` / `README`. No encoder, no BCJ filter TUs, no
  MicroLZMA users, no build system files.

## Use case in this firmware

- XZ/LZMA2 decoding for wrapped archive content on the SD card
  (`.tar.xz` comics, XZ-wrapped single files) behind the bounded
  first-party wrapper `libs/ra8_unarch/` (`ra8_unarch_xz.h`).
- Integrity claim category: data-handling (decompression of untrusted
  local SD-card payloads). The threat model is app crash / resource
  exhaustion, not RCE; the wrapper's job is reject-and-continue.

## Configuration (compile-time, decode-only)

The vendored sources are built against the first-party porting header
`libs/ra8_unarch/inc/xz_config.h` (upstream stays byte-identical):

- `XZ_DEC_PREALLOC` only: decoder state and LZMA2 dictionary are allocated
  once at `xz_dec_init` from a caller-provided scratch buffer through the
  zero-heap bump arena (`ra8_unarch_xz_pool.h`, NASA P10 Rule 3 -- this
  firmware traps `_sbrk`). `XZ_DEC_DYNALLOC` is deliberately NOT enabled:
  it would size allocations from hostile header values. `XZ_DEC_SINGLE`
  is not enabled: no consumer decodes from a fully-resident input buffer.
- `XZ_USE_CRC64`: verifies the CRC64 integrity check `xz`(1) emits by
  default; CRC32 is always verified. SHA-256-checked streams are rejected
  (`XZ_OPTIONS_ERROR`) rather than decoded unverified (`XZ_DEC_ANY_CHECK`
  is NOT enabled).
- No BCJ filters (`XZ_DEC_ARM` etc.): e-reader content is data, not
  executables; filtered streams are rejected fail-closed.

## Qualification basis

Accepted as-is per IEC 61508-3 Section 7.4.2.12 and DO-178C Section
12.1.4:

- **Service history**: the identical decoder has shipped in the Linux
  kernel since 2.6.38 (2011) where it decompresses untrusted kernel
  images, initramfs archives, and squashfs data on billions of devices.
- **Open-source community process**: maintained by the XZ Utils author
  with a public issue tracker and mailing list.
- **Bug tracker review**: the 2024 XZ Utils backdoor (CVE-2024-3094)
  affected liblzma's build system and sshd integration, NOT the XZ
  Embedded project, which is a separate, tiny, decode-only codebase with
  no build scripts vendored here; the vendored tree is fingerprint-pinned
  to a public upstream commit and hash-verified by the SBOM gate.

## Risk mitigation

- Exercised only on locally staged SD-card content; no network payload
  feeds it.
- Every decode runs behind `libs/ra8_unarch/src/ra8_unarch_xz.c`, which
  charges the unified decompression-limits policy
  (`libs/ra8_core/inc/ra8_decomp_limits.h`): per-unit output cap,
  compression-ratio bound (decompression bombs), and a decode-loop
  iteration budget. Dictionary memory is bounded by the caller scratch;
  a stream declaring a larger dictionary is rejected before allocation.
- Fuzzed continuously: the `fuzz_ra8_unarch_xz` libFuzzer harness (ASan +
  UBSan) drives hostile streams through the wrapper in the nightly fuzz
  sweep, and the committed hostile corpus in `tests/test_ra8_unarch_xz.c`
  asserts each rejection class.
- MC/DC exemption: per `CLAUDE.md`, `libs/third_party/` SOUP is exempt
  from in-repo MC/DC re-test; the first-party wrapper and policy code
  around it are held to the full 100 percent reachable MC/DC bar.

## Deviations / patches

None. The vendored tree is unmodified (byte-identical to upstream commit
`ae63ae3a`); all porting is done in the first-party `xz_config.h`.

## Last review date

- Reviewed: 2026-07-16
- Integrity sentence corrected (#627): 2026-08-04. It cited a transcribed
  aggregate hash "recorded in `sbom_registry.py`" -- the field #538 removed,
  and a value no gate parsed.
- Expected re-review by: 2027-07-16
