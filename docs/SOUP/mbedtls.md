# SOUP Justification: Mbed TLS

Per IEC 61508-3 Section 7.4.2.12 and DO-178C Section 12.1.4, this document
records the qualification basis for accepting Mbed TLS into this firmware
as Software Of Unknown Provenance (SOUP).

## Component identity

- **Name**: Mbed TLS
- **Version**: 4.1.0 (per `include/mbedtls/build_info.h`
  MBEDTLS_VERSION_STRING_FULL = "Mbed TLS 4.1.0"; ChangeLog confirms the
  4.1.0 branch released 2026-03-31).
- **Upstream URL**: https://github.com/Mbed-TLS/mbedtls
- **Local path**: `libs/third_party/mbedtls/`

## Provenance

- **Origin**: TrustedFirmware.org / Linaro project (originally PolarSSL,
  acquired by ARM as Mbed TLS, then transferred to TrustedFirmware).
- **License**: Dual Apache-2.0 OR GPL-2.0-or-later (`LICENSE`); we
  consume it under Apache-2.0.
- **How it entered our tree**: Vendored snapshot of the upstream
  Mbed TLS `development` branch shortly after the 4.1.0 release.
  Resolved (#548) to commit `d12fbb991c0822f347bbc569badef904629ce605`
  (2026-04-29): 252 of the 256 vendored files are byte-identical to it.
  It is a post-tag snapshot, not the tag: the vendored tree carries
  `ChangeLog.d/` fragments and the `ssl_tls12_client.c` RSA-PSS SigAlg fix
  that v4.1.0 does not. The remaining four files are enumerated under
  "Deviations / patches" below.
  - **Precision of the pin.** Seven commits in the 294 between `v4.1.0` and
    upstream `development` carry this exact set of 252 blobs (files outside
    the vendored subset changed between them, files inside it did not), so the
    subset cannot single one out. `d12fbb99` is the earliest and the closest
    to the 2026-05-01 vendor-in. The recorded claim -- every vendored file is
    byte-identical to upstream at `d12fbb99` -- is exact; what the evidence
    does not establish is that no other commit would satisfy it too.

- **Release basis**: `v4.1.0` (`0fe989b6b514`) plus 72 commits. The vendored
  pin `d12fbb991c08` is a post-tag development snapshot, not the release.
  `v4.1.0` is the newest named release the pin descends from: measured
  2026-09-17 against upstream's tag graph, the pin is 72 commits ahead of that
  tag and none behind it. Declared in `scripts/gen/sbom_registry.py`, published
  in the SBOM as `ra8:releaseBasis` / `ra8:commitsAfterRelease`, and held to
  this sentence by `scripts/checks/check_soup_upstream.py`, so the tag, the
  distance and this prose cannot drift apart one edit at a time.

## Upstream currency (measured 2026-09-17)

Recorded here because #804's table reads as though this tree were behind
4.1.0. It is not: it is 72 commits past it, and the 3.x to 4.x breaking
migration that issue sizes as the risk has already happened. What is actually
open is a move within the 4.x line, measured against upstream's tag graph on
2026-09-17:

- `v4.1.1` (released 2026-07-07) is the 4.1 LTS patch release, and it is
  neither an ancestor nor a descendant of our pin. The comparison diverges:
  260 commits on that branch our snapshot does not carry, 56 development
  commits it does.
- `v4.2.0` (released 2026-07-07) is a descendant of our pin, 211 commits
  ahead, so moving to it is a fast-forward along the same line rather than a
  branch change.
- No advisory conclusion is drawn here. `osv-scan` was not run for this
  record, so whether either release fixes anything that affects our build
  options stays open on #804.

## Use case in this firmware

- TLS record layer and X.509 handling, consumed via `libs/ra8_tls/` by
  `examples/ek_ra8d2/hw_pending/tls_client`, and driven raw (no facade) by
  `examples/_unsupported/threadx_https_client`.
- **Not** consumed by `libs/ra8_ota/`. The OTA module takes a
  dependency-injected crypto interface and never links `ra8_tls` or Mbed TLS;
  `libs/ra8_ota/inc/ra8_ota.h` states this at its seam. The only wiring today
  uses `ra8_rsip` SHA with a TODO for a tf-psa ECDSA backend.
- **Nothing TLS runs on hardware.** Both consumers cross-compile in CI (the
  `build-cross` gate builds the canonical matrix from
  `scripts/dev/ra8_apps.py`,
  and both apps' CMakeLists.txt force `-DRA8_USE_MBEDTLS=ON`), but neither
  carries a `hil.conf`: `tls_client` is `hw_pending` and
  `threadx_https_client` is `_unsupported`. The C6 Wi-Fi path is DHCP + ICMP
  only and has no TCP, so it has no transport for TLS either.
- Crypto primitives live in the sibling `tf-psa-crypto` package
  (separated upstream as of 4.x); see `docs/SOUP/tf-psa-crypto.md`.
- Integrity claim category: data-handling (TLS framing, certificate
  parsing).

## Qualification basis

Accepted as-is per IEC 61508-3 Section 7.4.2.12 and DO-178C Section
12.1.4:

- **Service history**: Mbed TLS / PolarSSL has been in continuous
  production use across embedded TLS deployments since 2009.
- **Open-source community process**: TrustedFirmware.org governance,
  documented Long-Term-Support branches, and a published security
  policy in `SECURITY.md`.
- **Vendor qualification data**: Mbed TLS has historically held PSA
  Certified Crypto API conformance evidence. Cited for context only;
  this project does not claim PSA Certified status.
- **Bug tracker review**: Advisories at
  https://github.com/Mbed-TLS/mbedtls/security/advisories reviewed;
  4.1.0 includes all currently published fixes affecting our build
  options.

## Risk mitigation

- `libs/ra8_tls/` is the intended policy chokepoint and `tls_client` uses
  it. Note the limits of that claim as it stands: the facade does not
  implement certificate pinning and never calls
  `mbedtls_ssl_conf_ciphersuites`, so "cipher suites and pinning are
  centrally enforced" would be false today -- it centralises session
  lifecycle and BIO wiring, not suite or trust policy. Nor is it the only
  door: `threadx_https_client` calls `mbedtls_ssl_*` directly.
- Exposure is bounded by the fact that no TLS code executes on hardware (see
  "Use case" above), not by a mitigation. Before any TLS consumer is
  hardware-validated, suite and trust policy have to be enforced somewhere
  real and every consumer routed through it.

## Deviations / patches

Four files, all consequences of vendoring a CMake-configured project into a
build that never runs its generators. Each is declared in
`scripts/gen/sbom_registry.py` and pinned by content in
`docs/sbom/upstream/mbedtls.manifest`; the other 252 vendored files are
verified byte-identical to the upstream pin on every CI run (#548).

1. `library/mbedtls_config_check_before.h`, `library/mbedtls_config_check_final.h`
   and `library/mbedtls_config_check_user.h` -- generated by upstream at
   configure time and vendored because the cross build does not run the
   generator. They have no upstream counterpart at any revision.
2. `library/.gitignore` -- upstream ignores those three generated headers as
   build output, so the ignore block is dropped (commit `f8f760c72`); keeping
   it would make the vendored headers untrackable.

No source file of Mbed TLS itself is modified.

## Last review date

- Reviewed: 2026-05-02
- Expected re-review by: 2027-05-02
