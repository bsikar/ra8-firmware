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
  it joins the runner pool: more hardware, more parallel CI. Two shapes are
  supported and can run side by side -- an autoscaling ARC pool on a k8s
  cluster, and a single container on any plain Docker host. See "The CI runner
  pool spans two hosts" below.
- **HIL bench** -- configures the bench Pi's J-Link, `rfp-cli`, serial console,
  smart-plug control, the ESP32-C6 build toolchain, and the Digilent Analog
  Discovery 2 logic analyzer used to probe the RA8 <-> C6 SPI lines.
- **Throwaway environments** -- create and configure new LXCs, including
  per-agent isolated dev containers.

## Layout

```
terraform/   creates machines + cluster resources (Proxmox LXC/VM, k8s/Helm)
ansible/     configures machines (ci_runner, ci_runner_docker, hil_bench,
             c6_toolchain, ad2_tools roles)
images/      the CI runner container image (devcontainer toolchain + runner)
```

## The CI runner pool spans two hosts

Both roles register runners against the same repository and both boot the same
`localhost/ra8-ci-runner:v2` toolchain image, so a job behaves identically
whichever host takes it. They differ only in shape:

| Role | Host | Shape | `runs-on:` it answers |
|---|---|---|---|
| `ci_runner` | k3s node | ARC scale set, pods, autoscaling 0..8 | `ra8-ci` |
| `ci_runner_docker` | any Docker host | one long-lived container | `ra8-ci`, `ra8-nas`, `[self-hosted, Linux, X64]` |

**Why two.** The build farm was one machine. The k3s node hosting the ARC pods
and the dev container where every agent runs `make ci` are guests on a single
10-core i5-12600K with ~30 vCPU committed across them, and it sat at a load
average near 20 with CI and local gate runs fighting for the same silicon.
Contention, not runner count, was the throughput ceiling; the fix is a second
machine, not more pods on the first.

**`ra8-ci` as a plain label is verified, not assumed.** `ra8-ci` is the ARC
runner *scale-set name*, and scale-set names and runner labels share one
namespace when GitHub resolves `runs-on:` -- ARC's own docs warn the two
interact. Rather than trust that, the first runner deployed under this role was
registered with the label and watched: it picked up `Unit tests (host)`
(`runs-on: ra8-ci`, `labels: ["ra8-ci"]`) one second after coming online and
finished green. A plain runner carrying the label therefore joins the existing
pool with **no workflow edit at all**. If a future GitHub change breaks that,
the fallback is already in place -- the runner also carries `ra8-nas`, so the
heavy jobs can be pinned with `runs-on: [self-hosted, ra8-nas]`.

The role's `self-hosted`/`Linux`/`X64` labels are added by the runner itself
and cannot be removed, which also makes the host eligible for the
`runs-on: [self-hosted, Linux, X64]` jobs (docs-publish, fuzz-nightly,
osv-scan).

### Storage: CI I/O is kept off a named pool, by assertion

The NAS this role was first deployed to has a **DEGRADED** 100T `raid-z2` pool:
a known backplane fault flagging "too many slow I/Os" on 7 of 11 members, with
zero read/write/checksum errors and no data errors. It is accepted and is not
this role's to repair -- but no build traffic belongs on it.

So the role does not *document* the carve-out, it *enforces* it. Before writing
anything it resolves the filesystem actually backing each path CI touches --
the runner root and the Docker daemon's data root, read from the daemon rather
than assumed -- and fails the play if either lands on a pool in
`ci_runner_docker_forbidden_pools` or under a mount in
`ci_runner_docker_forbidden_mounts`. A moved mountpoint or a re-pointed data
root is a failed deploy, not a silent relocation onto degraded spindles.

Everything the runner writes lands in exactly two places, both on the healthy
pool:

- `ci_runner_docker_root` (a dataset, default `/mnt/stripe/ci-runner`) -- the
  image archive, the runner distribution, its registration credentials, and the
  `_work` checkout where builds actually happen.
- the Docker data root -- image layers and the container's writable layer.

### What survives a TrueNAS SCALE upgrade

SCALE is an appliance: it rewrites its root filesystem on upgrade, so anything
under `/etc`, `/usr` or `/root` is temporary storage. This role writes **none**
of it -- no systemd unit, no package install, no file outside the pool. What
that buys:

- **Survives.** The dataset and everything in it: the runner binaries, the
  `.runner`/`.credentials` registration (so no re-registration and no
  credential is needed after an upgrade or a reboot), the `_work` tree, the
  image archive, and the rendered compose file. Also the dataset itself, which
  is created through the TrueNAS middleware (`midclt`) rather than behind its
  back, so the appliance knows it owns it.
- **Probably survives, not guaranteed.** The loaded Docker image and the
  container object. Both live in the Docker data root on a pool dataset, which
  a routine upgrade does not touch -- but a major migration of the Apps
  subsystem has reinitialised that dataset before. This is why the image
  archive is kept beside the runner home: recovery is `docker load` plus a
  re-run, which is what the role does anyway.
- **Does not survive, and does not need to.** Nothing. The role installs
  nothing on the appliance root by design.

So the recovery from any upgrade damage is the same single command as the
initial deploy: re-run the playbook. It is idempotent -- it re-loads nothing it
already has and re-registers nothing already registered.

One dependency is worth stating because it is easy to get wrong: on SCALE
`docker.service` is **not** systemd-enabled (`systemctl is-enabled docker`
reports `disabled`). The middleware starts it as part of bringing the Apps
subsystem up, which happens only while an apps pool is configured. The
container's `restart: unless-stopped` then brings the runner back by itself.
So "the runner returns after a reboot" is true *because* Apps is enabled on a
pool -- unset the apps pool and Docker never starts, and the runner never comes
back no matter what its restart policy says.

### Deploy and remove

```
cd infra/ansible

# deploy (and converge an existing deployment)
ansible-playbook -i inventory/hosts.ini playbooks/ci-runner-docker.yml

# remove: container down, runner deregistered from GitHub, image dropped,
# dataset destroyed. One command, nothing left behind.
ansible-playbook -i inventory/hosts.ini playbooks/ci-runner-docker.yml \
  -e ci_runner_docker_state=absent
```

Removal is a role path rather than a README snippet on purpose: a hand-written
teardown recipe drifts from the deploy it is supposed to undo, and the only
way to be sure a removal is complete is for the same file to own both halves.
Pass `-e ci_runner_docker_destroy_dataset=false` to keep the image archive for
a later redeploy.

### Resource caps

The runner is capped so the host stays useful for its actual job. The defaults
are sized for a 6-core/12-thread NAS with 62 GiB of RAM:

- **`ci_runner_docker_cpus: 8`** of 12 threads -- 4 physical cores plus their
  SMT siblings. Heavy gates stay genuinely parallel while 4 threads remain for
  the file services, ZFS transaction groups and the middleware. An uncapped
  runner wins every scheduling contest against SMB during a build, which turns
  a CI job into a NAS outage.
- **`ci_runner_docker_memory: 16g`** -- the ARC scale-set pods cap at 12 GiB
  and pass, so this is that with headroom for a whole job in one container. It
  cannot starve the ZFS ARC (~34 GiB here): Linux ZFS shrinks the ARC under
  memory pressure down to `arc_min` and grows it back afterwards.

The play reads the caps back out of the container's cgroup and **asserts** them
rather than trusting the compose file, because a cap that was silently ignored
is worse than one that was never set.

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
