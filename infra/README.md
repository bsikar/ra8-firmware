# infra/ -- reproducible rig as code

Provisions and configures every machine the project runs on -- dev boxes, CI
runners, and the hardware-in-the-loop (HIL) bench -- from code, so an
environment is recreated instead of hand-assembled.

## Quick start (clone -> deploy)

```
make infra-setup          # or: bash infra/bootstrap.sh
```

The bootstrap checks prerequisites, writes your **git-ignored** inventory, names
the fleet's machines in your `~/.ssh/config` (generated from `fleet.yml`, so
this machine can drive the estate without anyone hand-copying aliases -- see
`docs/CI_FLEET.md`), stores your GitHub token, and offers to run the deploy.
Nothing secret is ever committed -- your token lands in
`infra/ansible/private/` (git-ignored) or, if you run one, in OpenBao. That's
it: your machine joins as a CI runner pool.

## What it does

- **Dev box** -- one playbook installs the exact pinned toolchain, so `make ci`
  locally matches CI.
- **CI runner** -- point a spare machine (or a friend's server) at the repo and
  it joins the runner pool: more hardware, more parallel CI. Two shapes are
  supported and can run side by side -- an autoscaling ARC pool on a k8s
  cluster, and long-lived containers on any plain Docker host (including a
  Windows machine's WSL2 distro).
- **HIL bench** -- configures the bench Pi's J-Link, `rfp-cli`, serial console,
  smart-plug control, the ESP32-C6 build toolchain, and the Digilent Analog
  Discovery 2 logic analyzer used to probe the RA8 <-> C6 SPI lines.
- **Throwaway environments** -- create and configure new LXCs, including
  per-agent isolated dev containers.

## Layout

```
fleet.yml    THE declaration: one block per machine -- its ADDRESS (an IP or a
             resolvable name, never an ssh alias), what kind of host it is, how
             many runner instances, its CPU and memory per instance, its labels,
             its quiet-hours window. Everything below is derived from it, as is
             the ~/.ssh fragment `make infra-ssh-config` installs.
ansible/     configures machines (dev_box, ci_runner, ci_runner_docker,
             wsl_ci_host, fleet_capacity, hil_bench, c6_toolchain, ad2_tools)
images/      the CI runner container image (devcontainer toolchain + runner)
network/     the isolated ESP32-C6 bench LAN (FortiGate + OpenWrt AP)
```

`ansible/inventory/hosts.ini` is **generated** from `fleet.yml` on every
`make infra-*` run and is git-ignored; the committed half is
`ansible/inventory/host_vars/`, which holds structural facts about a machine
(where its runner tree lives, which pools CI must avoid) and never a capacity
knob. `scripts/checks/check_fleet_declaration.py` fails a `host_vars` file that
re-declares anything `fleet.yml` owns.

**How many machines there are, how each is sized, and which labels it carries
are questions only `fleet.yml` answers.** Nothing in this file restates them,
deliberately: a prose copy of a capacity figure is wrong the first time anyone
retunes a host and nothing notices.

**Adding a machine, retuning one, quiet hours, removing one:
[`docs/CI_FLEET.md`](../docs/CI_FLEET.md).** It also carries the sizing formula
and the measurements behind it, so a new host is sized by plugging in two
numbers rather than re-deriving anything.

## What is codified, and what is not

The rule this directory exists to serve is that **anything hand-installed is
lost at the next re-provision**. That makes the honest status per role part of
the documentation, not a footnote:

| Role / service | Status |
|---|---|
| `dev_box` (dev / verification box) | codified |
| `k3s_node` (cluster + `helm`) | codified |
| `openbao` (vault deployment) | codified |
| `ci_runner` (ARC runner pool) | codified |
| `ci_runner_docker` (Docker build host) | codified |
| `wsl_ci_host` + `ci_runner_docker` (Windows/WSL2 build host) | codified |
| `hil_bench`, `c6_toolchain`, `ad2_tools` (bench Pi) | codified |
| `network/` (bench LAN: FortiGate + AP) | codified |
| vault init / unseal / secrets (`scripts/secrets/`) | manual **by design** |
| Proxmox guest topology | **hand-built** (#500) |

Two rows deserve their exact wording. Vault initialisation and unsealing are
manual *by design*, not by omission: both produce secrets, and a playbook that
handles a root token can log one. The Proxmox row is a genuine open gap -- the
VM and LXC definitions the whole rig sits on exist only as live guest config.

### Order of operations on a bare cluster

`ci_runner` deploys ARC into "an existing k3s cluster" through helm, so it has
always had two prerequisites that lived nowhere: the cluster, and a `helm` root
could resolve. `k3s_node` is those prerequisites, and a host that declares both
plays runs them in that order:

```
make infra-apply HOST=k3s-pve
```

`PLAY=` on `scripts/dev/fleet.py` runs one on its own.

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
make infra-apply HOST=dev
```

Two of its tools are built from source, and that is a property of the
distribution rather than a preference. **cppcheck** is compared in `exact` mode
by the parity gate because neighbouring releases emit version-specific false
positives against this tree, and no Debian suite carries the pin at any version
string. The **second host-tool compiler arm** (#356) is a gcc major that Debian
12 does not package at all. Both are pinned by URL + sha256 and asserted
afterwards, the same discipline as every vendor download here. A first run
therefore takes tens of minutes; re-runs skip both builds once the pinned
versions are present.

The role reads the *other* pins rather than restating them -- lint and format
versions from `.devcontainer/Dockerfile`, the Unicorn pin from
`scripts/ci/unicorn_pin.sh`, and the two systemd units from the scripts that
generate them -- and finishes on `check_tool_versions.py --all`, the exact
assertion the `toolchain-parity` gate makes. A box that cannot reach parity
fails the play.

## The CI runner pool spans several hosts

Every runner role registers against the same repository and boots the same
toolchain image, so a job behaves identically whichever host takes it. They
differ only in shape and in where they run:

| Role | Host kind | Shape |
|---|---|---|
| `ci_runner` | k3s node | ARC scale set, pods, autoscaling from zero |
| `ci_runner_docker` | any Docker host | N long-lived containers |
| `wsl_ci_host` + `ci_runner_docker` | Windows machine, in WSL2 | N long-lived containers |

Instance counts, CPU and memory allocations, and the labels each host carries
are **not** properties of the roles: every one of them comes from that host's
block in `infra/fleet.yml`. See [`docs/CI_FLEET.md`](../docs/CI_FLEET.md).

**Every workflow targets `ra8-ci`.** The one exception is `hil.yml`, which
targets `[self-hosted, hil, ra8d2]` -- a physical-bench label naming the Pi with
the EK-RA8D2 attached, not a capacity pool. Nothing schedules against a bare
`[self-hosted, Linux, X64]`; those labels are added by the runner itself and
cannot be removed, and nothing targets them.

Per-host labels are escape hatches, not scheduling targets: they exist so a
specific host can be pinned or drained without editing every workflow.

**Why more than one host.** The build farm was one machine, and the k3s node
hosting the ARC pods shared its silicon with the dev container where every
agent runs `make ci`. Contention, not runner count, was the throughput ceiling;
the fix is more machines, not more pods on the first. Measured against the same
gate and the same commit, an otherwise-idle host runs the heavy cross-build
several times faster than a pod on the saturated node -- and that gap is
contention, not CPU.

**`ra8-ci` as a plain label is verified, not assumed.** `ra8-ci` is the ARC
runner *scale-set name*, and a scale-set name and a runner label are resolved
from the same `runs-on:` string -- so whether a plain runner carrying that
label joins the scale set's pool, shadows it, or is ignored is not something to
guess at. The first runner deployed under `ci_runner_docker` was registered
with the label and watched: it picked up an `ra8-ci` job within seconds of
coming online and has taken that work since. A plain runner carrying the label
therefore joins the existing pool with **no workflow edit at all**. If a future
GitHub change breaks that, the fallback is already in place -- each host also
carries a per-host label, so heavy jobs can be pinned to it.

### The Windows host runs several runners inside WSL2

`wsl_ci_host` prepares the distro; `ci_runner_docker` then deploys into it
**unmodified**. Keeping the platform-specific work in a separate role is what
stops the shared role growing a second personality per host.

The runner runs in WSL2 rather than on Windows because the toolchain image is
the toolchain: every pinned tool lives in the runner image, and the
`toolchain-parity` gate exists to fail a runner that drifts from those pins. A
native Windows runner would need a separately assembled toolchain -- exactly
the drift the gate is there to catch.

**The instance count is a memory result, not a core count.** Queue depth is the
fleet's bottleneck, not per-job latency, so a host with cores to spare runs
several independent runners rather than one runner with a very wide `-j`. The
bound is clang-tidy, which has been OOM-killed on a too-small share: cores
would divide further, memory is what says stop. Each instance gets its own
registration, home and `_work` tree, and is pinned to its own cpuset so `nproc`
inside it reports its real share and `make -j$(nproc)` cannot oversubscribe the
box. If clang-tidy is ever sharded, per-shard memory drops and another instance
becomes viable; re-measure then rather than assuming.

Some of the VM is left uncommitted on purpose: this machine has a discrete GPU
and will later host the GPU side of the Ethos-U55 / NPU workstream (#228) in
this same distro. GPU work and CI do not contend for an execution resource, but
they do contend for system RAM.

**Docker Engine in the distro, not Docker Desktop.** Docker Desktop is
installed on this machine and is left completely alone, but it cannot host this
runner: its daemon sits behind a Windows application that needs a logged-in
interactive session, so the runner would stop being able to start containers
the moment the owner logged out. That is not an unattended runner. Native
`docker-ce` runs under the distro's own systemd. Because the Desktop
integration also puts a `docker` shim on `PATH` from `/mnt/c`, the role
*asserts* that the binary in use is the distro's own -- a silent switch back
would reintroduce the GUI dependency with no other symptom.

**The build tree stays off `/mnt/c`.** The runner root is on the distro's ext4
root. `/mnt/c` is drvfs, a translation layer onto NTFS, and roughly an order of
magnitude slower for the many-small-files work a checkout and a build are;
putting `_work` there would hand back most of the CPU advantage this host was
added for.

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
  tunnelling endpoint does **not** answer on this host: a raw UDP/53 query to
  it times out while a public resolver answers in milliseconds over the same
  interface. Left generated, every name lookup in a job stalls for the full
  resolver timeout.

**The VM must be told not to die.** WSL reaps an unattended VM regardless of
what is running inside it, and this host was seen going `offline` with
`busy=true` -- GitHub still had a job assigned to a VM that had been shut down
underneath it. `vmIdleTimeout=-1` plus a blocking keep-alive in the autostart
task fixes it. The keep-alive matters: a task running `wsl -e true` starts the
VM and exits, and the VM is reaped moments later, which is an autostart that
reliably leaves the runner offline while looking configured.

**The clock may be wrong; it may not be non-monotonic.** A WSL2 VM does not
boot with a trustworthy clock. Observed here: the VM came up minutes fast, the
runner checked the tree out with those timestamps, `timesyncd` then stepped the
clock back, and make spent the rest of the job reporting `Clock skew detected.
Your build may be incomplete`. That is not cosmetic -- make's up-to-date
decisions are timestamp comparisons. Docker is therefore ordered after the
clock has synchronised, and since the containers are `restart: unless-stopped`,
the daemon's start time is exactly when this host begins accepting jobs.

Ordering was necessary and **not sufficient** (#509). It fixes the step that
happens at boot and says nothing about one hours later -- which is what this
host was measured doing. Scanning recent completed workflow runs for a step
whose recorded `completed_at` precedes its own `started_at` found several, every
one of them on this machine's runners and none anywhere else in the fleet.
Every full-size event is a backward step of the same magnitude, and the size
does *not* grow with the gap between events -- so it is not drift, it is two
time sources disagreeing by a fixed offset and taking turns: the Windows host,
whose clock a WSL2 guest takes and which WSL re-asserts periodically, against
NTP inside the distro.

So the role does two things. `chrony` replaces `timesyncd`, configured to step
only while starting up and to **slew** every correction after that -- a build
farm wants a clock that is briefly wrong but monotonic, because every gate
whose contract is a duration (libFuzzer's `-max_total_time`, `timeout-minutes`,
any benchmark) is measured on it. And the Windows clock is **asserted** rather
than assumed: correcting it needs an elevated Windows action that WSL interop
cannot perform, so the play fails with the exact `w32tm` commands instead of
converging on a host that will resume stepping. `make ci-gate GATE=runner-clock`
re-reads the fleet from the Actions API and is what proves it converged.

**What survives a reboot, stated precisely.** WSL does not start distros on
boot, so a Windows Scheduled Task (`ra8-wsl-ci-runner-autostart`) starts it;
that starts systemd, which starts docker, which starts the runner containers.

*Proven.* Running that task against a stopped distro brings the entire chain
back -- distro, systemd, docker, every container -- in seconds. Verified by
`wsl --shutdown` followed by `schtasks /Run` and nothing else touching the
machine.

*Not proven, and cannot be as configured.* That the trigger fires after an
actual power cycle. A WSL distro is registered under the owning account's HKCU,
so a boot-time task running as SYSTEM cannot start this distro at all -- `wsl
-d <distro>` in SYSTEM's context does not find it. Running a boot-triggered
task as the owning user instead requires "run whether user is logged on or
not", which stores that user's Windows password, and this is a personal
machine. This host also has `AutoAdminLogon` disabled, so after a reboot it
sits at the login screen with its runners offline until somebody logs in.

Closing that gap needs an **owner decision, not more code**: either enable
automatic logon for the account, or supply a credential so the task can be
recreated as `ONSTART` with `/RU` + `/RP`. Until then reboot recovery here is
manual, and the pool degrades onto its other hosts rather than breaking.

### Storage: CI I/O is kept off a named pool, by assertion

The NAS `ci_runner_docker` was first deployed to has a **DEGRADED** `raid-z2`
pool: a known backplane fault flagging "too many slow I/Os" on most members,
with zero read/write/checksum errors and no data errors. It is accepted and is
not this role's to repair -- but no build traffic belongs on it.

So the role does not *document* the carve-out, it *enforces* it. Before writing
anything it resolves the filesystem actually backing each path CI touches --
the runner root and the Docker daemon's data root, read from the daemon rather
than assumed -- and fails the play if either lands on a pool in
`ci_runner_docker_forbidden_pools` or under a mount in
`ci_runner_docker_forbidden_mounts`. A moved mountpoint or a re-pointed data
root is a failed deploy, not a silent relocation onto degraded spindles.

Everything the runner writes lands in exactly two places, both on the healthy
pool: `ci_runner_docker_root` (a dataset holding the image archive, the runner
distribution, its registration credentials, and the `_work` checkout where
builds actually happen) and the Docker data root (image layers and the
container's writable layer).

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
`docker.service` is **not** systemd-enabled. The middleware starts it as part
of bringing the Apps subsystem up, which happens only while an apps pool is
configured. The container's `restart: unless-stopped` then brings the runner
back by itself. So "the runner returns after a reboot" is true *because* Apps
is enabled on a pool -- unset the apps pool and Docker never starts, and the
runner never comes back no matter what its restart policy says.

### Deploy and remove

```sh
# deploy (and converge an existing deployment). Drains the host first: a
# converge recreates containers, which would cancel the jobs they hold.
make infra-apply HOST=truenas

# just the drain script and the quiet-hours timer -- touches no container
python3 scripts/dev/fleet.py apply truenas --tags capacity

# remove: containers down, runners deregistered from GitHub, image dropped,
# dataset destroyed. One command, nothing left behind.
make infra-remove HOST=truenas
```

Removal is a role path rather than a README snippet on purpose: a hand-written
teardown recipe drifts from the deploy it is supposed to undo, and the only
way to be sure a removal is complete is for the same file to own both halves.
Pass `-e ci_runner_docker_destroy_dataset=false` to keep the image archive for
a later redeploy.

### Resource caps

The runner is capped so the host stays useful for its actual job, and both caps
come from the host's `fleet.yml` block rather than from this file. The
reasoning behind them does not change when the numbers do:

- **CPU** is capped below the host's thread count so heavy gates stay genuinely
  parallel while enough threads remain for the file services, ZFS transaction
  groups and the middleware. An uncapped runner wins every scheduling contest
  against SMB during a build, which turns a CI job into a NAS outage.
- **Memory** is sized to hold a whole job in one container, with headroom over
  what the ARC scale-set pods pass on. It cannot starve the ZFS ARC: Linux ZFS
  shrinks the ARC under memory pressure down to `arc_min` and grows it back
  afterwards.

The play reads the caps back out of the container's cgroup and **asserts** them
rather than trusting the compose file, because a cap that was silently ignored
is worse than one that was never set.

## The legacy `k3s-runner-*` pool is retired (#502)

Before ARC, CI ran on hand-registered GitHub runners installed as bare systemd
services on the k3s node itself. They were never provisioned from this tree --
no role ever created them -- which is precisely why they outlived their
purpose: nothing in the repo described them, so nothing in the repo retired
them either. This section exists so that cannot happen a second time.

The ARC migration moved the core workflows to `ra8-ci` but deliberately left
`docs-publish`, `fuzz-nightly` and `osv-scan` on `[self-hosted, Linux, X64]`
"until their tools are confirmed in the image". **That condition was never
revisited**, so three low-frequency workflows kept the legacy runners alive on
an already-saturated node -- and made "retire the legacy pool" a trap, since
deregistering them would have left those three permanently unrunnable. A
temporary label with an unexamined condition on it is how a retired pool stays
alive indefinitely.

The condition was then checked rather than assumed, against a live pod:
`docs-publish` needs `dot` (graphviz, already in the image) and the pinned
doxygen (served from the hostPath tool cache, sha256-verified on a cold cache);
`fuzz-nightly` needs a clang that links `-fsanitize=fuzzer`, already in the
image; `osv-scan` fetches `osv-scanner` per job, version- and sha256-pinned in
the workflow. Nothing had to be added to the image. All three now target
`ra8-ci`, the registrations are deleted, and the `actions.runner.*` units are
stopped, disabled and removed along with their drop-in directories and the
runner installs under the node's home directory.

One in-repo default depended on that pool and moved with it.
`scripts/ci/monitor.sh runner-status` -- the zero-quota fallback that reads job
outcomes straight off a runner's `_diag` logs -- pointed at the legacy runner
tree on the k3s node. It needs a LONG-LIVED runner, which an ephemeral ARC pod
can never be, so it now reads a container runner's `_diag` (on a dataset
outside the container). That is a fix rather than a relocation: by the end the
legacy pool only ever ran those three workflows, so it could not show a
firmware result at all.

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
it declares a glibc dependency for Qt GUI binaries a headless bench never
installs, while the library the bench actually uses needs an older symbol
version and runs fine on the bench's glibc. The role's defaults carry the
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
copy of the same installs. It requests the newer gcc pair from apt only where
apt has a candidate for it -- Debian has none -- and leaves the verdict to the
parity check either way, which still fails loudly when neither path produced a
pinned compiler. It installs the pinned **doxygen** release the same way the
Dockerfile does (download, sha256, `/usr/local/bin`), because
`toolchain-parity` now compares that version too: it did not, which is why a
major doxygen drift stayed invisible to the one gate whose job is catching
exactly that (#522). The pin is asserted only on the architectures the
Dockerfile pins it for -- doxygen publishes no official linux-arm64 binary, so
an arm64 `make ci` container keeps apt's.

The **image `make ci` boots** is pinned by the same file and now derived from
it: `scripts/ci/devcontainer_image.sh` stamps a sha256 of the whole
`.devcontainer/` build context onto the image as a label and compares it back,
so a cached image built from a different Dockerfile is rebuilt rather than
reused. `make ci` calls it on every run -- which is what covers the Mac, where
no Ansible play ever lands -- and the `dev_box` role calls the same script so
`make infra-apply HOST=dev` leaves the box warm and asserts, with
`check_runner_image_deps.py`, that every tool the gates declare resolves inside
it. Before that, the box booted a stale image under a newer tree and reported
gates red that passed natively on the same commit (#521).

## Secrets

Only generic, shareable structure is committed -- no real hostnames, IPs, or
tokens. Real inventory lives in git-ignored `ansible/private/`; secrets live in
OpenBao and are fetched at run time. Copy an `*.example` file, fill in the real
values, and run the playbook.
