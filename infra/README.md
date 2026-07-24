# infra/ -- reproducible rig as code

This tree provisions and configures every machine the project runs on -- dev
boxes, the CI runners, and the hardware-in-the-loop (HIL) bench Pi -- from code,
so an environment can be recreated byte-for-byte instead of hand-assembled.

Its reason to exist is the failure it prevents: for a long time the CI runner
and the developer box were provisioned by hand and quietly drifted apart, so a
gate could pass locally yet fail in CI (a missing `g++-14` sank the coverage
gate for hours, invisible from a local run). The pinned toolchain now lives in
one place -- `.devcontainer/Dockerfile` -- and every box here derives from it,
so they cannot diverge.

## What you can do with it

- **Stand up a dev box.** One playbook installs the exact pinned toolchain, so
  `make ci` locally is the same environment CI uses.
- **Add a CI runner.** Point a spare machine (or a friend's server) at the repo
  and it joins the runner pool -- more hardware, more parallel CI, no shared SSH.
- **Rebuild the HIL bench.** The bench Pi's J-Link, `rfp-cli`, serial-console
  and smart-plug control are configured from a role, not a wiki page.
- **Spin up throwaway environments.** New LXCs (including per-agent isolated dev
  containers) are created and configured end to end.

## Layout

```
infra/
  terraform/     creates machines + cluster resources (Proxmox LXC/VM, k8s/Helm)
  ansible/       configures machines (toolchain, dev, ci-runner, hil-bench roles)
  images/        the CI runner container image (devcontainer toolchain + runner)
```

## The single source of truth for the toolchain

`.devcontainer/Dockerfile` pins every host tool (compilers, linters, formatters)
to an exact version. Three things consume those pins so they can never disagree:

- `make ci` on a workstation runs inside that image.
- the CI runner pods run that same image (see `images/`).
- `scripts/ci/provision_runner.sh` installs those exact versions onto a
  bare-metal host, and the `toolchain-parity` CI gate fails loudly if any box
  drifts from the pins.

## Secrets and private data never live here

Everything committed here is generic and shareable -- there are no real
hostnames, IP addresses, tokens, or credentials in this tree.

- **Structure and logic** (roles, playbooks, modules, example inventories with
  placeholder values) are committed.
- **Real inventory** (actual hosts) lives under `ansible/private/`, which is
  git-ignored.
- **Secrets** (runner registration token, smart-plug credentials, and so on)
  live in OpenBao and are fetched at run time; nothing secret is ever written to
  disk in this tree or committed to the repository.

Copy an `*.example` file, drop the real values into the git-ignored `private/`
location or OpenBao, and run the playbook.

## Status

Built in phases. See the top-level project tracker for what is landed versus
planned; each subdirectory's own `README.md` documents how to run it.
