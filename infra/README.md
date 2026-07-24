# infra/ -- reproducible rig as code

Provisions and configures every machine the project runs on -- dev boxes, CI
runners, and the hardware-in-the-loop (HIL) bench -- from code, so an
environment is recreated instead of hand-assembled.

## What it does

- **Dev box** -- one playbook installs the exact pinned toolchain, so `make ci`
  locally matches CI.
- **CI runner** -- point a spare machine (or a friend's server) at the repo and
  it joins the runner pool: more hardware, more parallel CI.
- **HIL bench** -- configures the bench Pi's J-Link, `rfp-cli`, serial console,
  and smart-plug control.
- **Throwaway environments** -- create and configure new LXCs, including
  per-agent isolated dev containers.

## Layout

```
terraform/   creates machines + cluster resources (Proxmox LXC/VM, k8s/Helm)
ansible/     configures machines (toolchain, dev, ci-runner, hil-bench roles)
images/      the CI runner container image (devcontainer toolchain + runner)
```

## Toolchain source of truth

`.devcontainer/Dockerfile` pins every host tool. `make ci`, the CI runner pods,
and `scripts/ci/provision_runner.sh` all consume those pins, and the
`toolchain-parity` gate fails if any box drifts from them.

## Secrets

Only generic, shareable structure is committed -- no real hostnames, IPs, or
tokens. Real inventory lives in git-ignored `ansible/private/`; secrets live in
OpenBao and are fetched at run time. Copy an `*.example` file, fill in the real
values, and run the playbook.
