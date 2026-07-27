# infra/ -- reproducible rig as code

Provisions and configures every machine the project runs on -- dev boxes, CI
runners, and the hardware-in-the-loop (HIL) bench -- from code, so an
environment is recreated instead of hand-assembled.

## Quick start (clone -> deploy)

```
git clone <this-repo> && cd ra8-firmware
make infra-setup          # or: bash infra/bootstrap.sh
```

The bootstrap checks prerequisites, writes your **git-ignored** inventory and
GitHub token, and offers to run the deploy. Nothing secret is ever committed --
your token lands in `infra/ansible/private/` (git-ignored) or, if you run one,
in OpenBao. That's it: your machine joins as a CI runner pool.

## What it does

- **Dev box** -- one playbook installs the exact pinned toolchain, so `make ci`
  locally matches CI.
- **CI runner** -- point a spare machine (or a friend's server) at the repo and
  it joins the runner pool: more hardware, more parallel CI.
- **HIL bench** -- configures the bench Pi's J-Link, `rfp-cli`, serial console,
  smart-plug control, the ESP32-C6 build toolchain, and the Digilent Analog
  Discovery 2 logic analyzer used to probe the RA8 <-> C6 SPI lines.
- **Throwaway environments** -- create and configure new LXCs, including
  per-agent isolated dev containers.

## Layout

```
terraform/   creates machines + cluster resources (Proxmox LXC/VM, k8s/Helm)
ansible/     configures machines (ci_runner, hil_bench, c6_toolchain,
             ad2_tools roles)
images/      the CI runner container image (devcontainer toolchain + runner)
```

## Vendor artifacts that cannot be fetched unattended

Every downloadable a role installs is pinned by version *and* sha256, and the
role asserts the installed version afterwards. Most vendor packages are served
openly and need nothing from a human.

A few are not, and those roles **fail the play with download instructions**
rather than skipping -- a bench that provisioned "successfully" without the
tool would be a lie. Today that is Digilent WaveForms (`libdwf`, the Analog
Discovery 2 capture SDK) in the `ad2_tools` role: every direct URL 403s behind
a click-through licence gate. Download it once from the page named in the
failure message, drop it in `/tmp` or `~/Downloads`, and re-run -- the role
adopts it into `/var/cache/ra8-bench/`, checks the deb's sha256 and embedded
version against the pins, installs it, and smoke-tests the instrument. Nothing
durable is ever left in `/tmp`.

That deb is also the one package installed by **extraction** rather than apt:
it declares `libc6 (>= 2.41)` for Qt GUI binaries a headless bench never
installs, while the library the bench actually uses tops out at `GLIBC_2.38`
and runs fine on the bench's glibc 2.39. The role's defaults carry the
`objdump` that establishes this, so the workaround cannot be mistaken for a
hack and "fixed" back into an install apt refuses.

The same holds for `JLinkExe` and `rfp-cli` on the HIL bench, which the
`hil_bench` role asserts are present instead of installing.

## Toolchain source of truth

`.devcontainer/Dockerfile` pins every host tool. `make ci`, the CI runner pods,
and `scripts/ci/provision_runner.sh` all consume those pins, and the
`toolchain-parity` gate fails if any box drifts from them.

## Secrets

Only generic, shareable structure is committed -- no real hostnames, IPs, or
tokens. Real inventory lives in git-ignored `ansible/private/`; secrets live in
OpenBao and are fetched at run time. Copy an `*.example` file, fill in the real
values, and run the playbook.
