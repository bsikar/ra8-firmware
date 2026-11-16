# ra8-firmware Documentation

This directory holds the firmware architecture, style, toolchain, hardware,
and third-party reference documentation for `bsikar/ra8-firmware`.

## Firmware Documentation Index

- **Architecture and platform**
  - [`ARCHITECTURE.md`](ARCHITECTURE.md) -- system architecture and tier boundaries
  - [`MODULES.md`](MODULES.md) -- library and module structure
  - [`RING_AND_WORLD.md`](RING_AND_WORLD.md) -- architectural rings and TrustZone worlds
  - [`MEMORY_MAP.md`](MEMORY_MAP.md) -- MRAM, SRAM, ITCM, DTCM, and flash layout
  - [`PORTS.md`](PORTS.md) -- RTOS, USB, networking, and TLS ports
  - [`APP_API.md`](APP_API.md) -- public application boundary
  - [`DUAL_CORE.md`](DUAL_CORE.md) -- Cortex-M85 and Cortex-M33 coordination
  - [`ROOT_OF_TRUST.md`](ROOT_OF_TRUST.md) -- secure boot and key-storage architecture
  - [`DRIVER_STATUS.md`](DRIVER_STATUS.md) -- peripheral driver maturity
  - [`VENDOR_BLOBS.md`](VENDOR_BLOBS.md) -- vendor binary policy
- **Language, style, and tooling**
  - [`STYLE_GUIDE.md`](STYLE_GUIDE.md) -- C23 and Doxygen style guide
  - [`ZIG_MIGRATION.md`](ZIG_MIGRATION.md) -- incremental Zig migration plan
  - [`ZIG_ABI.md`](ZIG_ABI.md) -- Zig and C ABI rules
  - [`TOOLCHAIN.md`](TOOLCHAIN.md) -- pinned compilers and build tools
  - [`PYTHON_ENVIRONMENTS.md`](PYTHON_ENVIRONMENTS.md) -- repository-local Python setup
  - [`IDE.md`](IDE.md) -- editor integration
  - [`DEBUG.md`](DEBUG.md) -- flashing and debug workflows
  - [`HARDWARE_BRINGUP.md`](HARDWARE_BRINGUP.md) -- EK-RA8D2 hardware bring-up notes
  - [`STACK_USAGE.md`](STACK_USAGE.md) -- static stack-frame budgets
  - [`FUZZING.md`](FUZZING.md) -- host fuzzing harnesses
  - [`PERFORMANCE.md`](PERFORMANCE.md) -- benchmark methodology
  - [`EPUB_CONFORMANCE.md`](EPUB_CONFORMANCE.md) -- reader book format conformance
  - [`CITATION_POLICY.md`](CITATION_POLICY.md) -- hardware manual and symbol citations
  - [`AI_ATTRIBUTION_POLICY.md`](AI_ATTRIBUTION_POLICY.md) -- authorship provenance policy
  - [`CONTENT_LICENSING.md`](CONTENT_LICENSING.md) -- third-party content licensing
  - [`ACRONYMS.md`](ACRONYMS.md) -- glossary of acronyms
  - [`DOCS.md`](DOCS.md) -- documentation layout and `just/docs.just` recipes
  - [`ROADMAP.md`](ROADMAP.md) -- historical HAL completion record
  - [`tools_inventory.md`](tools_inventory.md) -- inventory of `tools/` and `scripts/`
  - [`index.md`](index.md) -- documentation landing summary
- **Scope and subdirectories**
  - [`CERTIFICATION_SCOPE.md`](CERTIFICATION_SCOPE.md) -- authoritative certification-scope boundary
  - [`SOUP/`](SOUP/) -- third-party software-of-unknown-provenance records
  - [`adr/`](adr/) -- architectural decision records
  - [`reference/`](reference/) -- committed datasheets and Hardware User's Manual PDFs
  - [`design/`](design/), [`diagrams/`](diagrams/), [`formats/`](formats/), [`pinouts/`](pinouts/), [`sbom/`](sbom/), [`badges/`](badges/)

## CI and Host-Build Gate Manuals

CI, static-analysis, annotation-gate, and macOS host-build manuals live under
`docs/` in `bsikar/ra8ci`:

- `docs/STATIC_ANALYSIS.md` in `bsikar/ra8ci`
- `docs/ANNOTATIONS.md` in `bsikar/ra8ci`
- `docs/MACOS_HOST_BUILDS.md` in `bsikar/ra8ci`

## Qualification Documentation (YouTrack KB)

The qualification basis and the planning, specification, design, verification,
accomplishment, and tool-qualification documents live in the `RA8FW` YouTrack
knowledge base under parent article `RA8FW-A-2` (*Firmware Qualification
Documentation*):

- `RA8FW-A-3` -- Qualification Basis: IEC 61508 SIL 3 / DO-178C Level B (QUALIFICATION_ROADMAP)
- `RA8FW-A-4` -- Plan for Software Aspects of Certification (PSAC)
- `RA8FW-A-5` -- Software Development Plan (SDP)
- `RA8FW-A-6` -- Software Verification Plan (SVP)
- `RA8FW-A-7` -- Software Configuration Management Plan (SCMP)
- `RA8FW-A-8` -- Software Quality Assurance Plan (SQAP)
- `RA8FW-A-9` -- Software Requirements Specification (SRS)
- `RA8FW-A-10` -- Software Design Description (SDD)
- `RA8FW-A-11` -- Software Verification Cases & Procedures (SVCP)
- `RA8FW-A-12` -- Software Verification Results (SVR)
- `RA8FW-A-13` -- Software Accomplishment Summary (SAS)
- `RA8FW-A-14` -- Tool Qualification Dossier (TOOL_QUALIFICATION)
