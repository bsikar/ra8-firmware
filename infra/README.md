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
  cluster, and long-lived containers on any plain Docker host (including a
  Windows machine's WSL2 distro). See "The CI runner pool spans three hosts"
  below.
- **HIL bench** -- configures the bench Pi's J-Link, `rfp-cli`, serial console,
  smart-plug control, the ESP32-C6 build toolchain, and the Digilent Analog
  Discovery 2 logic analyzer used to probe the RA8 <-> C6 SPI lines.
- **Throwaway environments** -- create and configure new LXCs, including
  per-agent isolated dev containers.

## Layout

```
ansible/     configures machines (dev_box, ci_runner, ci_runner_docker,
             hil_bench, c6_toolchain, ad2_tools roles)
images/      the CI runner container image (devcontainer toolchain + runner)
network/     the isolated ESP32-C6 bench LAN (FortiGate + OpenWrt AP)
```

## What is codified, and what is not

The rule this directory exists to serve is that **anything hand-installed is
lost at the next re-provision**. That makes the honest status per host part of
the documentation, not a footnote:

| Host / service | Role | Status |
|---|---|---|
| dev / verification box | `dev_box` | codified |
| k3s cluster + `helm` | `k3s_node` | codified |
| OpenBao vault (deployment) | `openbao` | codified |
| ARC runner pool | `ci_runner` | codified |
| second build host (Docker) | `ci_runner_docker` | codified |
| Windows/WSL2 build host | `wsl_ci_host` + `ci_runner_docker` | codified |
| HIL bench Pi | `hil_bench`, `c6_toolchain`, `ad2_tools` | codified |
| bench LAN (FortiGate + AP) | `network/` | codified |
| vault init / unseal / secrets | `scripts/secrets/` | manual **by design** |
| Proxmox guest topology | -- | **hand-built** (#500) |

Two rows deserve their exact wording. Vault initialisation and unsealing are
manual *by design*, not by omission: both produce secrets, and a playbook that
handles a root token can log one. The Proxmox row is a genuine open gap -- the
VM and LXC definitions the whole rig sits on exist only as live guest config.

### Order of operations on a bare cluster

`ci_runner` deploys ARC into "an existing k3s cluster" through helm, so it has
always had two prerequisites that lived nowhere: the cluster, and a `helm` root
could resolve. `k3s_node` is those prerequisites.

```
cd infra/ansible
ansible-playbook -i inventory/hosts.ini playbooks/k3s-node.yml   # cluster + vault
ansible-playbook -i inventory/hosts.ini playbooks/ci-runner.yml  # then ARC
```

k3s is pinned by release version *and* by the sha256 of the release binary, and
the upstream installer is fetched to disk and checksummed rather than piped
into a shell -- the same discipline the devcontainer applies to every download.
When upstream re-cuts `get.k3s.io` the checksum stops matching and the role
fails with the new digest, which is what a pin is for.

The vault is deployed but never initialised or unsealed. The role reports which
of the two the operator still owes it; the steps are in
`scripts/secrets/README.md`.

## The dev box

The machine agents run `make ci` / `make ci-native` on. It is **not** a CI
runner -- it never joins the Actions pool -- so it lives in its own inventory
group and its own playbook:

```
cd infra/ansible
ansible-playbook -i inventory/hosts.ini playbooks/dev-box.yml
```

Two of its tools are built from source, and that is a property of the
distribution rather than a preference. **cppcheck 2.13.0** is compared in
`exact` mode by the parity gate because neighbouring releases emit
version-specific false positives against this tree -- Debian ships 2.10,
Homebrew ships 2.21, and no Debian suite carries the pin at any version string.
**gcc 14.2.0** is the second host-tool compiler arm (#356) and is not packaged
on Debian 12 at all. Both are pinned by URL + sha256 and asserted afterwards,
the same discipline as every vendor download here.

A first run therefore takes tens of minutes. Re-runs skip both builds once the
pinned versions are present.

The role reads the *other* pins rather than restating them -- lint and format
versions from `.devcontainer/Dockerfile`, the Unicorn pin from
`scripts/ci/unicorn_pin.sh`, and the two systemd units from the scripts that
generate them -- and finishes on `check_tool_versions.py --all`, the exact
assertion the `toolchain-parity` gate makes. A box that cannot reach parity
fails the play.

## The CI runner pool spans three hosts

Every role registers runners against the same repository and boots the same
`localhost/ra8-ci-runner:v2` toolchain image, so a job behaves identically
whichever host takes it. They differ only in shape and in where they run:

| Role | Host | Shape | `runs-on:` it answers |
|---|---|---|---|
| `ci_runner` | k3s node | ARC scale set, pods, autoscaling 0..8 | `ra8-ci` |
| `ci_runner_docker` | any Docker host | one long-lived container | `ra8-ci`, `ra8-nas`, `[self-hosted, Linux, X64]` |
| `wsl_ci_host` + `ci_runner_docker` | Windows machine, in WSL2 | three long-lived containers | `ra8-ci`, `ra8-win`, `[self-hosted, Linux, X64]` |

**Why more than one.** The build farm was one machine. The k3s node hosting the
ARC pods and the dev container where every agent runs `make ci` are guests on a
single 10-core i5-12600K with ~30 vCPU committed across them, and it sat at a
load average near 20 with CI and local gate runs fighting for the same silicon.
Contention, not runner count, was the throughput ceiling; the fix is more
machines, not more pods on the first.

**The third host is the fastest one, and it was measured rather than assumed.**
`win-ci` is a Ryzen 9 7900X -- 12 physical Zen 4 cores against the 12600K's 6
performance cores, and unlike the k3s node it is not sharing them with a dev
container. Same gate, same commit, same toolchain image, same 8-CPU allocation
the NAS runner uses:

| gate | win-ci | truenas (NAS) | pve1 (ARC pod) |
|---|---|---|---|
| `build-cross`, all 218 apps, 8 CPUs | **258s** | 808s | 1689s |
| `tidy` (clang-tidy), full width | **96s** | -- | ~981s (contended) |

That is **3.1x the NAS and 6.5x a pve1 pod** on the heavy cross-build, on an
otherwise idle host, with all 218 apps passing. The pve1 figure is not a slow
CPU so much as a drowning one: that node runs at load average 77-100 on 16
vCPU, so its pods are contending, which is the entire reason more hosts were
added rather than more pods.

**`ra8-ci` as a plain label is verified, not assumed.** `ra8-ci` is the ARC
runner *scale-set name*, and a scale-set name and a runner label are resolved
from the same `runs-on:` string -- so whether a plain runner carrying that
label joins the scale set's pool, shadows it, or is ignored is not something to
guess at. The first runner deployed under this role was registered with the
label and watched: it picked up `Unit tests (host)` (`runs-on: ra8-ci`, job
`labels: ["ra8-ci"]`) one second after coming online and finished green, and
has taken `ra8-ci` work continuously since. A plain runner carrying the label
therefore joins the existing pool with **no workflow edit at all**. If a future
GitHub change breaks that, the fallback is already in place -- the runner also
carries `ra8-nas`, so the heavy jobs can be pinned with
`runs-on: [self-hosted, ra8-nas]`.

**What the second host bought.** Same job, same commit, truenas container
against a pve1 ARC pod. The pve1 column is the status quo being fixed -- pods
contending on a saturated host -- not an isolated benchmark, which is exactly
the number that matters:

| Job | truenas | pve1 |
|---|---|---|
| `Cross-build all apps` | 808s | 1689s / 1201s |
| `ra8_emulator boot smoke` | 556s | 2500s / 2436s |
| `Pre-commit gate suite` | 787s | 1331s / 1009s |
| `clang-tidy` | 329s / 331s | 982s / 981s |
| `MC/DC coverage gate` | 237s | 674s / 771s |
| `Coverage (gcovr 90/80)` | 118s | 407s / 429s |
| `Unit tests (host)` | 83s | 355s / 348s |

The `Cross-build all apps` row is the cleanest of these: both numbers are two
attempts of the *same workflow run* on the same commit, so the only variable is
which host picked the job up.

The role's `self-hosted`/`Linux`/`X64` labels are added by the runner itself
and cannot be removed, which also makes the host eligible for the
`runs-on: [self-hosted, Linux, X64]` jobs (docs-publish, fuzz-nightly,
osv-scan).

### The Windows host runs three runners inside WSL2

`wsl_ci_host` prepares the distro; `ci_runner_docker` then deploys into it
**unmodified**. Keeping the platform-specific work in a separate role is what
stops the shared role growing a second personality per host.

The runner runs in WSL2 rather than on Windows because the toolchain image is
the toolchain: every pinned tool lives in `localhost/ra8-ci-runner:v2`, and the
`toolchain-parity` gate exists to fail a runner that drifts from those pins. A
native Windows runner would need a separately assembled toolchain -- exactly
the drift the gate is there to catch.

**This host is sized to take the largest share of the fleet's work.** It is the
fastest machine available and is barely used interactively, so the WSL VM is
capped at 22 of 24 threads and 26 of 31 GiB -- enough for an idle Windows
desktop and no more. Both are role variables, so the judgement is reversible in
one place.

**Three runners, and three is a memory result.** Queue depth is the fleet's
bottleneck, not per-job latency, so a host with cores to spare runs several
independent runners rather than one runner with a very wide `-j`. The count is
bounded by clang-tidy, which has been OOM-killed at 8 GiB: 26/3 = 8.7 GiB per
instance clears that, 26/4 = 6.5 GiB does not. Cores would divide fine at four
-- memory is what says three. Each instance gets its own registration, home and
`_work` tree, and is pinned to its own cpuset so `nproc` inside it reports its
real share and `make -j$(nproc)` cannot oversubscribe the box. If clang-tidy is
ever sharded, per-shard memory drops and a fourth becomes viable; re-measure
then rather than assuming.

Roughly 5 GiB of the VM is left uncommitted on purpose: this machine has an
RTX 5070 and will later host the GPU side of the Ethos-U55 / NPU workstream
(#228) in this same distro. GPU work and CI do not contend for an execution
resource, but they do contend for system RAM.

**Docker Engine in the distro, not Docker Desktop.** Docker Desktop is
installed on this machine and is left completely alone, but it cannot host this
runner: its daemon sits behind a Windows application that needs a logged-in
interactive session, so the runner would stop being able to start containers
the moment the owner logged out. That is not an unattended runner. Native
`docker-ce` runs under the distro's own systemd. Because the Desktop
integration also puts a `docker` shim on `PATH` from `/mnt/c`, the role
*asserts* that the binary in use is the distro's own -- a silent switch back
would reintroduce the GUI dependency with no other symptom.

**The build tree stays off `/mnt/c`.** `ci_runner_docker_root` is
`/opt/ra8-ci-runner` on the distro's ext4 root. `/mnt/c` is drvfs, a
translation layer onto NTFS, and roughly an order of magnitude slower for the
many-small-files work a checkout and a build are; putting `_work` there would
hand back most of the CPU advantage this host was added for.

**Mirrored networking needs a route fix here, and that is not optional.** The
machine is multi-homed: an isolated bench LAN with **no uplink**, and the
owner's Wi-Fi. Mirrored networking copies the Windows routing table into the
distro, and the bench LAN's DHCP-supplied default gateway arrives with the
*lower* metric -- so out of the box every packet the runner sends goes into a
black hole. Windows itself is unaffected because it fails over, which is why
this reads as a working machine right up until a job runs. Two things fix it,
both inside the distro, and neither touches the owner's network:

- `ra8-wsl-route-pin` finds the default gateway that actually carries traffic
  (probed by IP, before DNS can be trusted) and pins it below every mirrored
  route. A **timer** re-asserts it, because mirrored networking re-syncs the
  Windows routing table on every host network change -- a boot-only fix would
  let a Wi-Fi roam break a job already in flight.
- `generateResolvConf=false` plus a resolv.conf the role owns. WSL's DNS
  tunnelling endpoint (`10.255.255.254`) does **not** answer on this host: a
  raw UDP/53 query to it times out while `1.1.1.1` answers in milliseconds over
  the same interface. Left generated, every name lookup in a job stalls for the
  full resolver timeout.

**The VM must be told not to die.** WSL reaps an unattended VM regardless of
what is running inside it, and this host was seen going `offline` with
`busy=true` -- GitHub still had a job assigned to a VM that had been shut down
underneath it. `vmIdleTimeout=-1` plus a blocking keep-alive in the autostart
task fixes it. The keep-alive matters: a task running `wsl -e true` starts the
VM and exits, and the VM is reaped moments later, which is an autostart that
reliably leaves the runner offline while looking configured.

**The clock must be right before the runners start.** A WSL2 VM does not boot
with a trustworthy clock. Observed here: the VM came up ~4 minutes fast, the
runner checked the tree out with those timestamps, `timesyncd` then stepped the
clock back, and make spent the rest of the job reporting `Clock skew detected.
Your build may be incomplete`. That is not cosmetic -- make's up-to-date
decisions are timestamp comparisons. Docker is therefore ordered after
`systemd-time-wait-sync`, and since the containers are `restart:
unless-stopped`, the daemon's start time is exactly when this host begins
accepting jobs.

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
the `dev_box` role and `scripts/ci/provision_runner.sh` all consume those pins,
and the `toolchain-parity` gate fails if any box drifts from them.

`provision_runner.sh` is the one definition of "bring a bare-metal host to the
pinned toolchain", so the `dev_box` role calls it rather than carrying a second
copy of the same installs. It requests the `gcc-14`/`g++-14` pair from apt only
where apt has a candidate for it -- Debian has none -- and leaves the verdict to
the parity check either way, which still fails loudly when neither path produced
a pinned compiler.

## Secrets

Only generic, shareable structure is committed -- no real hostnames, IPs, or
tokens. Real inventory lives in git-ignored `ansible/private/`; secrets live in
OpenBao and are fetched at run time. Copy an `*.example` file, fill in the real
values, and run the playbook.
