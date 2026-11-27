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
- **Local path**: none. Pinned in `build.zig.zon` as the upstream
  `nimble_1_10_0_tag` tarball (url + Zig content hash) and fetched into the
  Zig package cache by `cmake/zig_package.cmake`.

## Provenance

- **Origin**: Apache Software Foundation, Apache Mynewt project.
- **License**: Apache-2.0 (`LICENSE` and `NOTICE`).
- **How it entered our tree**: Vendored from the upstream release tag
  until RA8FW-385 (2026-10-02), which replaced the vendored copy with the
  pinned upstream tarball after re-checking every file. All 615 vendored files (614 regular files plus the one
  `porting/npl/riot/include/npl_syscfg/npl_sycfg.h` symlink) are
  byte-identical to `nimble_1_10_0_tag`. The vendored subset is
  `nimble/` (475 files), `porting/` (131) and nine repository-root files
  (`LICENSE`, `NOTICE`, `README.md`, `RELEASE_NOTES.md`, `version.yml`,
  `CODING_STANDARDS.md`, `.clang-format`, `.clang-format-ignore`,
  `.gitignore`).
- **What the subset drops, and why**: the upstream `apps/`
  directory (163 files) was never vendored, and 212 further files were
  pruned because no RA8 build can reach them -- `babblesim/` (76, the
  Zephyr BSIM shim, which carries its own `core_cm4.h`), `tools/` (41,
  Python host utilities), `.github/` (40, upstream CI and Nordic nRF5x
  Mynewt targets), `docs/` (26), `targets/` (19, Nordic nRF5340 Mynewt
  targets and their linker scripts), `ext/` (4), `qualification/` (1),
  and `.asf.yaml`, `.mailmap`, `.rat-excludes`, `Doxyfile`,
  `repository.yml`. None of those paths lie under any include directory
  `cmake/nimble.cmake` publishes, none is named by any file outside the
  vendored tree, and none is reached by the `nimble/host` + `porting/` +
  `nimble/transport` surface RA8FW-270 will compile. Verified three ways
  before deleting: no reference to any pruned path anywhere in the repo,
  no pruned path under a published include dir, and no `NOLINT` or
  `cppcheck-suppress` directive inside the pruned files (so no
  suppression-ledger churn). `LICENSE` and `NOTICE` are kept because
  Apache-2.0 redistribution requires them; `README.md`,
  `RELEASE_NOTES.md` and `version.yml` because this document cites them
  for the component's version identity. The prune shrinks the OSV, SBOM
  and exemption surface by roughly a quarter: the compiled surface from the
  upstream dependency tarball is the host, GATT services, store/config,
  transport, and porting-layer subset in `cmake/nimble.cmake`. A future
  re-vendor must keep excluding `apps/` and the pruned directories above.
- **Previous pin**: `8b6f3e819118a1839e5f238bfe1797d64878dc3d`, a
  default-branch snapshot 42 commits past `nimble_1_9_0_tag`, recovered
  by tree fingerprinting rather than by tag. It was replaced because it
  resolved into four published CVEs (see "CVE monitoring"). Pinning a
  tagged release instead of a dev-branch snapshot also removes the need
  to fingerprint the tree to know what we ship.

## Use case in this firmware

**Upstream Apache NimBLE 1.10.0 host translation units are compiled and linked
into NimBLE firmware targets (RA8FW-270).**

- **Intended & actual role**: Bluetooth 5.4 host stack. The ESP32-C6 companion
  runs the BLE controller over `ra8_c6link` (`ESP_HCI_IF`); NimBLE runs the host
  and GATT services on the RA8D2 MCU, reached over `ra8_ble` through
  `port/nimble/`.
- **Actually compiled**: `cmake/nimble.cmake` compiles upstream
  `nimble/host/src/*.c` (excluding `ble_gatts_lcl.c`),
  `nimble/host/util/src/addr.c`, `nimble/host/services/{bas,dis,gap,gatt}`,
  `nimble/host/store/config`, `nimble/transport/src/{monitor,transport}.c`, and
  `porting/nimble/src/{endian,mem,nimble_port,os_mbuf,os_mempool,os_msys}.c`
  into `nimble_objs`, pre-empting upstream syscfg defaults via
  `port/nimble/inc/syscfg/syscfg.h` (`#include_next`) and redirecting startup
  GATT table allocations on freestanding `Generic` targets onto an 8 KiB static
  SRAM pool (`port/nimble/src/nimble_port_alloc.c`). `nimble_port_threadx`
  provides `port/nimble/src/ble_hci_ra8_ble.c`, `nimble_npl_threadx.c`, and
  `nimble_port_alloc.c`; C6 BLE apps additionally compile
  `port/nimble/src/ble_hci_c6link.c`.
- **Disabled features**: Legacy/SC SMP (`MYNEWT_VAL_BLE_SM_LEGACY=0`,
  `MYNEWT_VAL_BLE_SM_SC=0`, `MYNEWT_VAL_BLE_SM_BONDING=0`), ISO pools, and Mesh
  (`MYNEWT_VAL_BLE_MESH=0`) remain compiled out via `port/nimble/inc/syscfg/syscfg.h`.
- **Firmware & host test coverage**:
  - `examples/ek_ra8d2/hw_validated/c6/c6_ble_nimble` builds a connectable
    peripheral (GAP, GATT, BAS `0x180F` / `0x2A19`) over `ble_hci_c6link` and
    `ra8_c6link`.
  - `tests/wireless/src/test_app_c6_ble_nimble.c` exercises real `nimble/host`
    GAP advertising, synthetic LE Connection Complete, ATT MTU exchange, and
    disconnect re-advertising over `ra8_c6_model`.
  - `examples/_unsupported/threadx_nimble_peripheral` retains its compile-only
    reference build against real `ble_hs_cfg` and `nimble_port_init()`.
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
- Host stack integration tests (`test_app_c6_ble_nimble`) and bounded HCI
  event sizing (`static_assert(MYNEWT_VAL(BLE_TRANSPORT_EVT_SIZE) >= 257)`)
  validate host/transport boundaries prior to live ESP32-C6 HIL testing.

## Deviations / patches

None. The build uses the pinned upstream release tarball as is. While it
was vendored, the tree was byte-identical to the tag, re-verified
file-by-file at the 1.10.0 re-vendor and again (615/615) when RA8FW-385
moved it to `build.zig.zon`. Omission is not
deviation: upstream `apps/` and the 212 unbuildable files listed under
"Provenance" are absent, and every file that is present matches the tag
byte for byte.

One historical deviation is worth recording, because it was undocumented
and because of how it arose. Under the previous 1.9.0+dev pin, a
tree-wide `esp32/` path-rename pass (commit `a823419b9`, cleaning up
after a deleted first-party spike) also rewrote a documentation URL
inside the then-vendored NimBLE `README.md`
(`.../esp-idf/en/latest/esp32/api-reference/...` ->
`.../esp32c6/api-reference/...`). That one-line edit made this document's
"byte-identical" claim, and the matching claims in
`THIRD_PARTY_LICENSES.md` and the SBOM, false from that commit onward.
It was cosmetic -- an upstream doc link, no code -- but the lesson is
that a repo-wide rename must exclude `libs/third_party/`. The 1.10.0
re-vendor restored upstream content, so the claim holds again.

That lesson is now a mechanism rather than a convention.
`scripts/gen/gen_sbom.py` re-derives a SHA-256 over this component's whole
vendored tree on every run -- 615 files, by sorted component-relative path,
git mode and content -- and `gen_sbom.py --check` in the `sbom` gate fails
naming the component the moment any of those bytes change. The drift above
went unnoticed because the SBOM's `aggregate_sha256` was a hand-transcribed
literal that nothing ever computed, and NimBLE did not even carry one; a
value re-derived from the tree is the only kind that can disagree with the
tree. A repeat of `a823419b9` now fails at the gate instead of quietly
falsifying this section.

A digest over our own tree would still not have caught the drift at
vendor-in -- it would have hashed the rewritten URL faithfully and reported
clean forever. So the claim above is also checked against upstream itself:
`docs/sbom/upstream/nimble.manifest` recorded the blob SHA-1 upstream
publishes for each of the 615 vendored files at `nimble_1_10_0_tag`, and the
`soup-upstream` gate compared our index against it on every push. With the
tree gone both checks are moot: the Zig content hash in `build.zig.zon` pins
the exact upstream bytes, and a fetch that does not match it fails.

## CVE monitoring

The pinned commit is queried against OSV.dev weekly by
`.github/workflows/osv-scan.yml` (commit-range GIT queries via
`scripts/checks/osv_scan.sh`); a published advisory affecting the pin
fails the scheduled run.

That gate fired on 2026-07-27 against the previous pin. The four
advisories, the code each one lives in, and their status here:

| CVE | Vulnerable code | In a build of ours? | Status |
| --- | --- | --- | --- |
| CVE-2026-45811 | `nimble/transport/socket/src/ble_hci_socket.c`, `ble_hci_sock_rx_msg` -- unchecked HCI event copy in the POSIX **socket** transport | No: our transport is `port/nimble/src/ble_hci_ra8_ble.c`, and `nimble/transport/socket/` is never built (`MYNEWT_VAL_BLE_TRANSPORT_LL__socket=0`) | Fixed in the pin |
| CVE-2026-45815 | `nimble/host/src/ble_gattc.c`, `ble_gattc_read_mult_cb_var` -- reachable assertion parsing an ATT Read Multiple Variable Response | **Yes** -- compiled in `nimble/host/src`; fixed in 1.10.0 | Fixed in the pin |
| CVE-2026-45816 | `nimble/host/src/ble_sm.c`, `ble_sm_ltk_req_rx` -- NULL dereference on an LE Long Term Key Request naming an unknown connection handle | Compiled in `nimble/host/src` with SMP disabled (`MYNEWT_VAL_BLE_SM_LEGACY=0`, `MYNEWT_VAL_BLE_SM_SC=0`); also fixed in 1.10.0 | Fixed in the pin |
| CVE-2026-46452 | `nimble/host/mesh/src/proxy_msg.c`, `bt_mesh_proxy_msg_recv` -- unbounded SAR reassembly append | No: BLE Mesh is excluded (`MYNEWT_VAL_BLE_MESH=0`, mesh sources omitted from `cmake/nimble.cmake`) | Fixed in the pin |

The four upstream fixes are commits `dcc4e4f0`, `fae6a487`, `9448c5f4`
and `593f9522` respectively; all are ancestors of `nimble_1_10_0_tag`,
and none was an ancestor of the previous pin. OSV.dev resolves the pinned
commit clean.

The "in a build of ours?" column is exposure context, not the basis for
accepting the pin. Nothing here is waived: the fix is the version bump,
so that analysis would have to be wrong in all four rows before it
changed the outcome.

## Last review date

- Reviewed: 2026-11-27 (RA8FW-270 linked NimBLE 1.10.0 host/GATT/transport
  subset into C6 BLE firmware and host integration tests; pin unchanged at
  `nimble_1_10_0_tag`, `2026-07-28` CVE audit verified against active TUs)
- Expected re-review by: 2027-07-28
