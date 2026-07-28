# Security Policy

ra8-firmware is a bare-metal firmware project for the Renesas RA8 family
(RA8D2 / RA8P1). This document covers how to report a vulnerability and the
security posture of the project's build and CI infrastructure.

## Reporting a vulnerability

Please report security issues **privately**, not in a public issue or pull
request.

- Preferred: use GitHub's **private vulnerability reporting** for this
  repository (the **Security** tab -> **Report a vulnerability**). This opens
  a private advisory visible only to the maintainer.
- If private reporting is unavailable, open a minimal public issue that says
  only "security report -- please enable private contact" (no details), and
  the maintainer will follow up over a private channel.

Please include: the affected file(s) / component, a description of the issue,
and a proof-of-concept or reproduction if you have one. This is a personal
research project maintained on a best-effort basis; there is no formal SLA,
but reports are taken seriously and acknowledged as time permits.

## Build & CI infrastructure

CI runs on **self-hosted runners** (a Linux build box and a Raspberry Pi
hardware-in-the-loop rig with a wired EK-RA8D2). Because self-hosted runners
execute on the maintainer's own hardware, untrusted code must never run on
them. The following controls enforce that:

- **Fork pull requests do not run on the self-hosted runners.** Every
  self-hosted CI job is gated so it runs only for pushes and for pull requests
  originating from this repository itself, never from a fork
  (`github.event.pull_request.head.repo.full_name == github.repository`).
  In addition, the repository requires manual approval before any outside
  contributor's workflow can run at all. A fork PR therefore gets no CI on the
  maintainer's infrastructure unless the maintainer explicitly reviews the diff
  and approves it.
- **No `pull_request_target`.** No workflow combines elevated permissions with
  a checkout of pull-request head code.
- **Minimal token scope.** `GITHUB_TOKEN` defaults to read-only; workflows that
  need write access request it explicitly and narrowly.

Contributions are not expected. Viewing and forking the code is fine and
harmless; only a *running* workflow could touch the maintainer's machines, and
that path is closed by the controls above.

## Supply chain

Third-party ("SOUP") components are vendored at pinned versions under
`libs/third_party/` and documented under `docs/SOUP/`. The project maintains a
CycloneDX SBOM (`docs/sbom/`), runs a weekly `osv-scanner` CVE scan against it,
builds host tests under UBSan on every pull request, and runs a nightly
libFuzzer sweep over the parsers that ingest untrusted content (EPUB / image /
font / archive data). The memory-unsafe initial-access surface is compiled with
memory-safety warnings kept as hard errors rather than blanket-disabled.

## Scope & honesty

This is a hobby / research firmware codebase, not a certified product. Where the
code targets a safety or security bar (for example DO-178C / IEC 61508 coding
discipline, or a TrustZone root of trust), that intent is documented alongside
its current enforcement state; features that are scaffolded, fake, or not
yet hardware-validated are labelled as such in-tree. Do not assume a security
control is active on real silicon unless the code and its tests say so.
