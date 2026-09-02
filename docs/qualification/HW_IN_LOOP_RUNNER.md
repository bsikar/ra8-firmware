# Legacy Workstation HIL Runner Setup (EK-RA8D2) -- RETIRED

**Status: retired.** The old workstation/Pi-runner bootstrap was removed. This
file remains only so old links do not lead operators to an executable, stale
setup procedure.

The current design has two separately managed hosts:

- `dev` runs the repo-scoped native Actions listener and builds with the pinned
  native toolchain.
- `star` is the instrument host; the existing `scripts/hil/` drivers reach it
  through the listener's dedicated SSH identity.

`infra/fleet.yml` declares the listener identity, labels, workflow ownership,
and its relationship to the bench. The `dev_box` Ansible role consumes that
declaration. The workflow itself is `.github/workflows/hil.yml` and schedules
only on `[self-hosted, hil, ra8d2]`; trusted same-repository PRs and matching
pushes may schedule it, while fork PRs are excluded.

Use these current references:

- [`../HIL_DEVELOPER_WORKFLOW.md`](../HIL_DEVELOPER_WORKFLOW.md) for running and
  diagnosing the hardware gate.
- [`../../infra/README.md`](../../infra/README.md) for provisioning, including
  first registration.
- [`../CI_FLEET.md`](../CI_FLEET.md) for the fleet declaration and ownership
  model.

Do not reconstruct the retired manual runner, `svc.sh`, sudoers, or direct
`config.sh` procedure from repository history. The managed route exists to
keep credentials, labels, service hardening, and the bench trust boundary from
drifting independently.
