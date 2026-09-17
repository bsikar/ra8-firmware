# ADR-0008: Vendoring-completeness contract for third-party components

- **Status**: Proposed -- the decision below is Brighton's to confirm; nothing
  is implemented by this record.
- **Date**: 2026-09-17
- **Issue**: #699 (Platform-arch (g): own-your-vendoring / add-a-first-party-or-vendored-lib cleanliness)
- **Parent epic**: #692
- **Verified against**: `dev` @ `1657296`

## Context

#699 asks for a *vendoring-completeness gate*: a vendored component directory
"with no `port/` shim, no `docs/SOUP/<name>.md`, no SBOM row, or no license
entry fails", so that a half-vendored component becomes a detectable state
rather than a thing a reviewer happens to notice. The issue is marked
DESIGN / PLANNING ONLY and is sequenced behind the filesystem work (#611), so
this record settles the contract's shape and leaves the landing to the owner.

Three of the issue's premises have moved since it was filed on 2026-08-09.
Everything below was read off `dev` @ `1657296`, not copied from the issue.

### Where vendored source actually lives

There is no top-level `third_party/` directory: `git ls-files third_party`
returns nothing. Vendored source sits under two roots, both recognised by
`scripts/gen/gen_sbom.py` (lines 303-307):

- `libs/third_party/` -- 12 component directories plus
  `libs/third_party/README.md`.
- `apps/shared_libs/third_party/` -- 5 component directories plus
  `apps/shared_libs/third_party/README.md`.

A third root, `tools/<tool>/third_party/`, is recognised by the same function
and has zero directories today.

### What the registry holds

`scripts/gen/sbom_registry.py` carries 20 entries: 18 vendored-source
components (including the nested `esp-hosted/protobuf-c`, vendored at
`libs/third_party/esp-hosted/common/protobuf-c`), one co-processor firmware
(`esp-hosted-mcu`, `scope="excluded"`, at `coprocessor/esp32c6/esp-hosted-mcu`)
and one bundled data asset (`fonts/Literata`, at
`libs/ra8_fonts/Literata-Regular.ttf`).

### What `port/` actually contains

Nine directories, not the eight the issue lists, and **there is no
`port/filex/`**: `port/threadx/`, `port/usbx/`, `port/levelx/`,
`port/netxduo/`, `port/nimble/`, `port/mbedtls/`, `port/esp-hosted/`,
`port/esp32_c6/`, `port/posix/`.

Two of those nine adapt no vendored tree at all: `port/esp32_c6/` is the
integration contract for firmware running on the companion radio, and
`port/posix/` is a first-party hosted filesystem adapter. So only **7 of the 18
vendored components have a `port/` shim**. The 11 without are `tf-psa-crypto`,
`litehtml`, `miniz`, `xz_embedded`, `stb`, `libwebp`, `tflite-micro`,
`flatbuffers`, `gemmlowp`, `ruy` and the nested `protobuf-c`: header-only
libraries, single-file amalgamations, and trees reached through an app-owned
facade such as `apps/shared_libs/webp/`.

### What is already gated, and what is not

Gated today, all three offline:

- `scripts/gen/gen_sbom.py` -- directory drift in **both** directions (a
  vendored directory with no registry entry fails, and a registry entry whose
  directory is absent fails), version-macro drift where a header carries one,
  named license-file presence, and a SHA-256 tree digest re-derived from disk
  on every run.
- `scripts/checks/check_soup_upstream.py` -- every vendored file is the blob
  its upstream project published, with deviations declared in the registry's
  `patched_files` / `local_files` rather than inferred.
- `scripts/checks/check_third_party_patches.py` -- each declared patch series
  reverses the checked-in bytes to the recorded upstream blob and reapplies.

Not gated by anything: the presence of a `docs/SOUP/<name>.md` qualification
record, the presence of a row in `THIRD_PARTY_LICENSES.md`, and the presence of
an index row in `docs/SOUP/README.md`. No script maps a registry key to either
document; the only mentions of those paths under `scripts/` are prose in
docstrings plus the per-path allowlists in
`scripts/checks/markdown_reference_policy.py`. A newly vendored component
therefore fails the SBOM gate until it has a registry entry, and then passes
every gate in the tree while carrying no qualification record at all -- which
matters because `CLAUDE.md` line 1249 rests the MC/DC exemption for both vendor
roots on exactly those records.

### Item 4 of #699 is already satisfied

`scripts/checks/check_no_silent_stubs.py` line 95 sets
`ROOTS = ("libs", "tools", "apps", "examples", "port")`, so shim sources under
`port/` are already scanned for the SHADOW / CANNED shapes. No work is owed
there.

### The key-to-document mapping is many-to-one by design

Four legitimate exceptions exist today, so a gate that derives the document
path from the registry key would be wrong four times out of twenty:

| Registry key | Qualification record | Why it differs |
|---|---|---|
| `esp-hosted` | `docs/SOUP/esp-hosted-host.md` | The vendored half is the host driver |
| `esp-hosted-mcu` | `docs/SOUP/esp-hosted.md` | The co-processor firmware half, qualified separately |
| `esp-hosted/protobuf-c` | folded into `docs/SOUP/esp-hosted-host.md` | Nested upstream submodule, licensed in its own right |
| `fonts/Literata` | none | `docs/SOUP/README.md` scopes itself to the two vendor roots |

And in the other direction, `docs/SOUP/vela.md` has no registry entry at all:
Arm Ethos-U Vela is a host build tool pinned by `pyproject.toml`, linked into
nothing.

## Decision drivers

1. Two of the four requirements the issue would gate are properties of a
   *stack*, not of a vendored component. A shim exists where there is a runtime
   seam to adapt. Requiring one universally fails 11 of 18 components, and the
   cheapest way to pass would be to write shims that adapt nothing -- which the
   No-Stubs Policy exists to forbid. A gate whose remedy is a stub is worse than
   no gate.
2. The document half is genuinely ungated and genuinely load-bearing: the MC/DC
   exemption in `CLAUDE.md` cites `docs/SOUP/`, and nothing proves the record it
   cites exists.
3. Any mapping from component to document must be *declared*, because it is
   already many-to-one in four places and those four are correct.

## Options

**A. Gate all four requirements exactly as #699 states them.** Rejected. It
fails 11 of 18 components on the shim requirement alone, and the only remedies
are an empty shim or a baseline with 11 rows, which is the prose it replaced.

**B. Gate the documents from declared fields; report the shim (recommended).**
Add two fields to each registry entry naming its qualification record and its
license-inventory row, and fail when either is missing, plus the
`docs/SOUP/README.md` index row. Shim presence becomes a third, *reported*
field carrying either a `port/<name>/` path or a one-line reason there is none,
so "reached through `apps/shared_libs/webp/`" is reviewable without being a
failure. The checks are peers of the directory-drift check already in
`scripts/gen/gen_sbom.py` rather than a new script, so `RA8_GATE_REGISTRY` and
`ci-parity` selftest coverage need no new entry.

**C. Leave it as prose (status quo).** The coupling stays checkable only by a
human reading three documents. Cheapest today; loses the "clean path is the only
path" property #699 is for.

**D. Derive the document path from the key, with an exceptions allowlist.**
Rejected. An allowlist of today's four exceptions is the same prose moved into
Python, and it grows silently each time a component is vendored nested or in two
halves.

## Decision

**Deferred to Brighton.** This record recommends option **B** and states the
constraint that decides against **A**: a `port/` shim is a property of a stack
with a runtime seam, and 11 of 18 vendored components legitimately have none, so
shim presence can be reported but not required.

Two questions are left open rather than answered here, because both are policy:

1. **Does the bundled font owe a qualification record?** `fonts/Literata` has an
   SBOM row and a `THIRD_PARTY_LICENSES.md` row but no `docs/SOUP/` record,
   because `docs/SOUP/README.md` scopes itself to the two vendor roots. It is
   parsed data, not compiled code, so the MC/DC exemption arguably does not
   reach it. Either the font is out of the catalog's scope on the record (a
   sentence in `docs/SOUP/README.md`), or it owes a short record like every
   other pinned artifact.
2. **Where does the "add a library" runbook live?** #699's first acceptance
   criterion wants it linked from `docs/STYLE_GUIDE.md`, which today contains no
   such link. Whether it is a new page under `docs/` or a section of
   `docs/STYLE_GUIDE.md` is a structure choice, not a technical one.

## Consequences

- If **B** is taken: one declared field per registry entry, and a newly vendored
  component cannot reach a green push while its qualification record, license
  row, or index row is missing. Adding a component costs three named edits that
  a machine names for you instead of three a reviewer has to remember.
- The four many-to-one mappings stay legal and become machine-readable for the
  first time, so the esp-hosted halves stop being a thing you have to know.
- Shim absence stays visible without becoming a failure, so the No-Stubs Policy
  and the completeness gate cannot pull in opposite directions.
- Nothing here unblocks #699's tier-placement gate (item 2), which still needs
  the tier taxonomy from children (a)/(b) before a gate can require a library to
  declare a tier.

## References

- Issue #699 (this record), parent epic #692, dependency #611.
- `scripts/gen/sbom_registry.py`, `scripts/gen/gen_sbom.py`,
  `scripts/checks/check_soup_upstream.py`,
  `scripts/checks/check_third_party_patches.py`,
  `scripts/checks/check_no_silent_stubs.py`.
- `docs/SOUP/README.md`, `THIRD_PARTY_LICENSES.md`,
  `docs/sbom/ra8-firmware.cdx.json`, `CLAUDE.md` line 1249.
