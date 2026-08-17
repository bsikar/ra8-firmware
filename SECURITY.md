# Security Policy

ra8-firmware is a bare-metal firmware project for the Renesas RA8 family
(RA8D2 / RA8P1).

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

## Why fork pull requests get no CI

CI runs on self-hosted runners -- the maintainer's own hardware, some of it
wired to a board it can flash. Untrusted code must therefore never execute on
them, and three controls enforce that:

- **Self-hosted jobs are gated to this repository.** Each runs only for pushes
  and for pull requests whose head repo is this repo
  (`github.event.pull_request.head.repo.full_name == github.repository`), never
  from a fork. An outside contributor's workflow additionally requires manual
  approval before it can run at all, so a fork PR gets no CI on the
  maintainer's infrastructure unless the maintainer reviews the diff first.
- **No `pull_request_target`.** No workflow combines elevated permissions with
  a checkout of pull-request head code.
- **Minimal token scope.** `GITHUB_TOKEN` defaults to read-only; a workflow
  that needs write access requests it explicitly and narrowly.

Contributions are not expected. Viewing and forking the code is harmless; only
a *running* workflow could touch the maintainer's machines, and that path is
closed.

## Supply chain

Third-party ("SOUP") components are vendored at pinned versions under
`libs/third_party/`, each with a written justification under `docs/SOUP/`, and a
CycloneDX SBOM is published under `docs/sbom/`. CI scans that SBOM for known
CVEs, builds the host tests under a sanitizer, and fuzzes the parsers that
ingest untrusted content -- EPUB, image, font and archive data, which is the
memory-unsafe initial-access surface. That surface is compiled with
memory-safety warnings kept as hard errors rather than blanket-disabled.

## Scope and honesty

This is a hobby / research firmware codebase, not a certified product. Where the
code targets a safety or security bar -- DO-178C / IEC 61508 coding discipline,
a TrustZone root of trust -- that intent is documented alongside its current
enforcement state, and anything scaffolded, faked, or not yet hardware-validated
is labelled as such in-tree. Do not assume a security control is active on real
silicon unless the code and its tests say so.
