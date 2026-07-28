# SOUP Justification: Apache NimBLE

Per IEC 61508-3 Section 7.4.2.12 and DO-178C Section 12.1.4, this document
records the qualification basis for accepting Apache NimBLE into this
firmware as Software Of Unknown Provenance (SOUP).

## Component identity

- **Name**: Apache NimBLE
- **Version**: 1.10.0, pinned at the release tag `nimble_1_10_0_tag` ==
  upstream commit `a7a156f28954819e158b62dd613008f22f9cf73b`
  (10 July 2026). The `version.yml` file reads `repo.version: 0.0.0`
  because upstream keeps that placeholder on its default branch, which
  the release tag points at; the 1.10.0 identity comes from the tag and
  from `RELEASE_NOTES.md`.
- **Upstream URL**: https://github.com/apache/mynewt-nimble
- **Local path**: `libs/third_party/nimble/`

## Provenance

- **Origin**: Apache Software Foundation, Apache Mynewt project.
- **License**: Apache-2.0 (`LICENSE` and `NOTICE`).
- **How it entered our tree**: Vendored from the upstream release tag.
  All 827 vendored files (826 regular files plus the one
  `porting/npl/riot/include/npl_syscfg/npl_sycfg.h` symlink) are
  byte-identical to `nimble_1_10_0_tag`. The vendored subset drops the
  upstream `apps/` directory (163 files) and nothing else.
- **Previous pin**: `8b6f3e819118a1839e5f238bfe1797d64878dc3d`, a
  default-branch snapshot 42 commits past `nimble_1_9_0_tag`, recovered
  by tree fingerprinting rather than by tag. It was replaced because it
  resolved into four published CVEs (see "CVE monitoring"). Pinning a
  tagged release instead of a dev-branch snapshot also removes the need
  to fingerprint the tree to know what we ship.

## Use case in this firmware

**No upstream NimBLE translation unit is compiled today.** This section
states the intended role and then, separately, what is actually in the
build, because the two are not yet the same and conflating them has
already misled one triage.

- **Intended role**: Bluetooth 5.4 host stack. The ESP32-C6 companion
  runs the BLE controller; NimBLE runs the host, reached over the
  `ra8_ble` HCI transport seam through `port/nimble/` (the ThreadX + HCI
  port).
- **Actually compiled**: `cmake/nimble.cmake` declares `nimble` as an
  INTERFACE target. It has no `target_sources` and no source list, so it
  contributes include directories only. `cmake/ra8_add_app.cmake` maps
  `USES nimble` onto `nimble_port_threadx`, whose entire source list is
  the two **first-party** files `port/nimble/src/ble_hci_ra8_ble.c` and
  `port/nimble/src/nimble_npl_threadx.c`. No `nimble/host/src`,
  `nimble/controller/src`, `nimble/transport/src`, mesh, audio or
  porting-layer source is in any image; the upstream transport core and
  NPL are stood in for by the weak no-op symbols declared in
  `port/nimble/inc/nimble_transport_stubs.h`.
- **Compiled surface from the vendored tree**: the single upstream header
  `nimble/nimble_npl.h`, reached via
  `port/nimble/inc/nimble_npl_threadx.h`. The `nimble/nimble_npl_os.h` it
  chains to resolves to this port's own shadowing copy under
  `port/nimble/inc/`, not to an upstream port. So what the vendored tree
  contributes to a linked image is a handful of NPL type and prototype
  declarations.
- **Corroboration**: `.github/misra-baseline.txt` lists only those two
  first-party `port/nimble/` files and no upstream NimBLE file, so no
  upstream TU has ever been through the compile-gated static analysis.
- `examples/_unsupported/threadx_nimble_peripheral` is the only app that
  links the port. It includes no upstream NimBLE header, is HW-blocked on
  the ESP32-C6 radio companion, and is an unvalidated scaffold (#286).
- Integrity claim category: none (no BLE-driven safety signal in the
  current firmware).

## Qualification basis

Accepted as-is per IEC 61508-3 Section 7.4.2.12 and DO-178C Section
12.1.4:

- **Service history**: Apache NimBLE has shipped in production BLE
  devices since 2017 (Apache Mynewt 1.0).
- **Open-source community process**: Apache Software Foundation
  governance, including formal voting on releases and a security
  reporting channel at security@apache.org. The four 2026 advisories
  below were disclosed on oss-security with fix commits and a release
  containing them, which is the CVE-response behaviour this basis
  assumes.
- **Bug tracker review**: Issues at
  https://github.com/apache/mynewt-nimble/issues reviewed. Every
  advisory published against NimBLE as of the review date below is fixed
  in the pinned version.

## Risk mitigation

- All BLE access is mediated through the NimBLE host via the
  `port/nimble/` port and the `ra8_ble` HCI transport seam, keeping a
  single integration boundary.
- The pin is a tagged release rather than a dev-branch snapshot, so the
  shipped code corresponds to something upstream voted on, and advisories
  can be attributed to it by version.
- Stack is not yet wired to any production-track example; introduction
  will require a fresh integration test pass. Because no upstream TU is
  compiled today, the first change that links `nimble/host/src` -- the
  ESP32-C6 BLE HCI seam (#493) -- is the point at which every host-stack
  advisory becomes live exposure in shipped code. Keeping this pin ahead
  of published fixes is therefore a precondition of that work, not a
  follow-up to it.

## Deviations / patches

None. The vendored tree is byte-identical to the pinned upstream release
tag (`apps/` omitted), re-verified file-by-file at the 1.10.0 re-vendor.

One historical deviation is worth recording, because it was undocumented
and because of how it arose. Under the previous 1.9.0+dev pin, a
tree-wide `esp32/` path-rename pass (commit `a823419b9`, cleaning up
after a deleted first-party spike) also rewrote a documentation URL
inside `libs/third_party/nimble/README.md`
(`.../esp-idf/en/latest/esp32/api-reference/...` ->
`.../esp32c6/api-reference/...`). That one-line edit made this document's
"byte-identical" claim, and the matching claims in
`THIRD_PARTY_LICENSES.md` and the SBOM, false from that commit onward.
It was cosmetic -- an upstream doc link, no code -- but the lesson is
that a repo-wide rename must exclude `libs/third_party/`. The 1.10.0
re-vendor restored upstream content, so the claim holds again.

That lesson is now a mechanism rather than a convention (#538).
`scripts/gen/gen_sbom.py` re-derives a SHA-256 over this component's whole
vendored tree on every run -- 827 files, by sorted component-relative path,
git mode and content -- and `gen_sbom.py --check` in the `sbom` gate fails
naming the component the moment any of those bytes change. The drift above
went unnoticed because the SBOM's `aggregate_sha256` was a hand-transcribed
literal that nothing ever computed, and NimBLE did not even carry one; a
value re-derived from the tree is the only kind that can disagree with the
tree. A repeat of `a823419b9` now fails at the gate instead of quietly
falsifying this section.

## CVE monitoring

The pinned commit is queried against OSV.dev weekly by
`.github/workflows/osv-scan.yml` (commit-range GIT queries via
`scripts/checks/osv_scan.sh`); a published advisory affecting the pin
fails the scheduled run.

That gate fired on 2026-07-27 against the previous pin (#508). The four
advisories, the code each one lives in, and their status here:

| CVE | Vulnerable code | In a build of ours? | Status |
| --- | --- | --- | --- |
| CVE-2026-45811 | `nimble/transport/socket/src/ble_hci_socket.c`, `ble_hci_sock_rx_msg` -- unchecked HCI event copy in the POSIX **socket** transport | No, and not under #493 either: our transport is `port/nimble/src/ble_hci_ra8_ble.c`, and `nimble/transport/socket/` is a host / simulator transport this firmware never builds | Fixed in the pin |
| CVE-2026-45815 | `nimble/host/src/ble_gattc.c`, `ble_gattc_read_mult_cb_var` -- reachable assertion parsing an ATT Read Multiple Variable Response | Not today; **yes** once #493 links `nimble/host/src`. Core GATT client, remotely triggerable (`AV:N/AC:L`) | Fixed in the pin |
| CVE-2026-45816 | `nimble/host/src/ble_sm.c`, `ble_sm_ltk_req_rx` -- NULL dereference on an LE Long Term Key Request naming an unknown connection handle | Not today; **yes** once #493 links `nimble/host/src`. Its threat model is a misbehaving controller, which is exactly what sits across our SPI HCI link to the C6 | Fixed in the pin |
| CVE-2026-46452 | `nimble/host/mesh/src/proxy_msg.c`, `bt_mesh_proxy_msg_recv` -- unbounded SAR reassembly append | No, unless BLE Mesh is adopted: the mesh sources are a separate opt-in subset that neither the current build nor #493 compiles | Fixed in the pin |

The four upstream fixes are commits `dcc4e4f0`, `fae6a487`, `9448c5f4`
and `593f9522` respectively; all are ancestors of `nimble_1_10_0_tag`,
and none was an ancestor of the previous pin. OSV.dev resolves the pinned
commit clean.

The "in a build of ours?" column is exposure context, not the basis for
accepting the pin. Nothing here is waived: the fix is the version bump,
so that analysis would have to be wrong in all four rows before it
changed the outcome.

## Last review date

- Reviewed: 2026-07-28 (bumped 1.9.0+dev -> 1.10.0 for the four 2026
  advisories; provenance re-verified against the release tag)
- Expected re-review by: 2027-07-28
