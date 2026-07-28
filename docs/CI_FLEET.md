# The CI fleet: declared, tuned, and scheduled

Every machine that runs CI for this project is declared in **one file**,
[`infra/fleet.yml`](../infra/fleet.yml). Adding a machine is adding a block to
it. Changing how much of a machine CI is allowed to use is changing a number in
that block. Nothing else has to be edited: the inventory, the playbook
selection, the transport and every role variable are derived from it.

This document is the runbook for the four things you will actually want to do:
add a host, retune one, give one quiet hours, and remove one. It ends with how
the numbers were arrived at, so a new machine can be sized without re-deriving
anything.

```sh
make infra-list                    what is declared, and how it is sized
make infra-show HOST=win-ci        one machine in full, with its derived vars
make infra-status                  what every host is running, right now
make infra-check HOST=truenas      DRY RUN -- report, change nothing
make infra-apply HOST=truenas      converge that machine to the declaration
make infra-scale HOST=win-ci N=1   live capacity change; shrinking DRAINS
make infra-remove HOST=truenas     tear it down
```

---

## 1. The one rule: a scale-down must DRAIN, never kill

This is the constraint everything else is built around, and it is not
theoretical.

**`docker stop` on a busy runner cancels the job it is running.** Not "risks
cancelling" -- cancels, deliberately, through three hops:

1. Our runner image sets `RUNNER_MANUALLY_TRAP_SIG=1` (the official
   `actions-runner` image's contract), so `run.sh` takes its
   `runWithManualTrap` path, whose `trap 'kill -INT -$PID' INT TERM` forwards a
   SIGTERM to the listener's process group as SIGINT.
2. `Runner.Listener` treats that as `ShutdownReason.UserCancelled`, cancels its
   message loop, and in the `finally` block calls
   `JobDispatcher.ShutdownAsync()`.
3. `ShutdownAsync()` calls
   `EnsureDispatchFinished(currentDispatch, cancelRunningJob: true)`, whose
   comment reads *"cancel running job when shutting down the runner"*. The job
   ends `Canceled`.

A longer `docker stop --time` buys nothing: the cancel is immediate and
intended, and the extra seconds are spent waiting for a job that has already
been told to stop. This fleet has already seen the consequence once -- WSL's
idle timeout reaped the whole VM out from under three live jobs, and GitHub
went on reporting those runners `busy=true` with orphaned work until it timed
out.

So [`scripts/ci/fleet_capacity.sh`](../scripts/ci/fleet_capacity.sh) **never
signals a busy runner**. It polls, and stops an instance in the moment it is
idle. A stopped container cannot be handed another job, so a host converges
downward one instance at a time as its jobs finish naturally.

"Is this instance busy" is answered **locally**: `Runner.Listener` spawns
exactly one `Runner.Worker` process per job, so `docker top <container>`
answers it with no GitHub API call and no credential on the host. That matters
-- the quiet-hours timer runs unattended on a machine that deliberately holds
no PAT.

If the deadline passes with instances still busy, the drain **reports what it
could not park and exits non-zero**. It does not force them. A scale-down that
kills jobs is worse than no scale-down at all.

The residual race -- a job assigned in the moment between the busy check
passing and the stop landing -- is one round trip wide and cannot be closed
without a credential on the host, so it is **detected**: after every stop, the
script reads that container's log for the stop's own time window and fails
loudly if a job started or was cancelled in it.

### A drain can take several job cycles, and that is correct

Nothing token-free can stop GitHub *assigning* work to a runner that is up --
only removing its labels does that, and that is an API call the machines with
quiet hours must not be able to make. So the drain waits for an idle moment and
takes it, and if the runner is handed another job first it simply waits again.

The poll interval is 3 seconds because a busy runner's idle gap is only a few
seconds wide: measured on the Windows host at a 15-second poll, an instance
finished one job and started the next between two checks, costing a whole extra
job cycle. Three seconds usually catches the first gap. On a saturated queue,
expect a drain to take minutes rather than seconds -- the NAS took 8m18s and
three job cycles -- and to report `NOT converged` rather than force anything if
it is still busy at its 90-minute deadline.

That is the trade the design makes on purpose: a slow scale-down that never
loses work, over a fast one that sometimes does.

**ARC (the k3s pool) is safe by construction** and needs none of this. Its
runners are ephemeral -- one job, then the pod exits -- and the controller only
deletes runners that hold no job, so lowering `maxRunners` never interrupts
work. The capacity script's `k8s` arm just moves the number.

---

## 2. The declaration

```yaml
sizing:
  build_parallelism: 4        # CMAKE_BUILD_PARALLEL_LEVEL, pinned by the workflows
  memory_per_instance_gb: 8   # clang-tidy's measured OOM ceiling

hosts:
  <name>:
    class: arc_k8s | docker_linux | docker_wsl | dev_box | hil_bench
    summary: "one line"
    connect:
      ssh: <alias or user@host>
      jump: <ssh alias>          # optional ProxyJump
      distro: Ubuntu             # docker_wsl only
      windows_user: sikar        # docker_wsl only
    provisions: [play, play]     # from `make infra-list`
    runners:                     # runner classes only
      name: <base>               # optional; defaults to the host name
      instances: 2
      cpus: 4
      memory_gb: 8
      pin_cpus: true             # docker classes
      cpu_request: 1             # arc_k8s only
      memory_request_gb: 2       # arc_k8s only
      labels: [ra8-ci, ra8-nas]
    budget:                      # what CI may use OF the machine
      mode: reserved | burst
      threads: 8
      memory_gb: 16
      swap_gb: 8                 # docker_wsl only (the VM's swap file)
    quiet_hours:                 # optional
      window: "18:00-23:59"
      days: "Fri,Sat,Sun"
      instances: 0
    sizing_note: >-              # required if the count departs from the formula
      why this host is not sized by the formula
```

### `class` picks the arithmetic, the plays and the variable mapping

| class | what it is | capacity is changed by |
|---|---|---|
| `arc_k8s` | an ARC scale set on a k8s cluster | patching `maxRunners` |
| `docker_linux` | long-lived runner containers on any Docker host | draining containers |
| `docker_wsl` | the same, inside a Windows machine's WSL2 distro | draining containers |
| `dev_box` | the shared verification box (not a runner) | n/a |
| `hil_bench` | the hardware-in-the-loop bench Pi (not a runner) | n/a |

### `budget.mode` picks which numbers must fit

- **`reserved`** -- the caps are kernel-enforced reservations, so
  `instances * cpus` and `instances * memory_gb` must fit the budget.
- **`burst`** -- the caps are ceilings a scheduler may oversubscribe, so the
  **requests** are what must fit. Only `arc_k8s` is honest as `burst`; applying
  the reserved arithmetic to a k8s scale set would fail a shape that is
  correct, which is how a gate teaches people to ignore it.

### What is NOT in the declaration

Structural facts about a machine -- where its runner tree lives, which of its
pools CI must never touch, whether it may hold a credential -- live in
`infra/ansible/inventory/host_vars/<host>.yml`. Those are properties of the
machine, not knobs.

The split is enforced: `check_fleet_declaration.py` fails a `host_vars` file
that re-declares any variable the declaration owns. Extra-vars beat
`host_vars`, so a duplicate would not change behaviour -- it would leave a
number in the tree that looks authoritative, that somebody will edit, and that
will do nothing.

---

## 3. Add a host

Worked example: a fourth machine arrives -- call it `bench-tower`, a
Ryzen 7 5800X with 16 threads and 32 GB, running Ubuntu, reachable as
`ssh tower`, that should give CI half of itself.

### Step 1 -- size it

```
instances = min( threads / 4 , memory_GB / 8 )
```

Half of 16 threads and half of 32 GB is a budget of 8 threads and 16 GB, so
`min(8/4, 16/8) = 2` instances, at `16/2 = 8` GB and `8/2 = 4` CPUs each.
Section 7 explains where the two divisors come from.

### Step 2 -- declare it

```yaml
  bench-tower:
    class: docker_linux
    summary: "Ryzen 7 5800X, Ubuntu: half the machine to CI"
    connect:
      ssh: tower
    provisions: [ci-runner-docker]
    runners:
      name: tower-ci
      instances: 2
      cpus: 4
      memory_gb: 8
      pin_cpus: true
      labels: [ra8-ci, ra8-tower]
    budget:
      mode: reserved
      threads: 8
      memory_gb: 16
```

`ra8-ci` is what puts it in the existing pool with no workflow edit -- that a
plain runner carrying the ARC scale set's name joins the same pool was verified
empirically, not assumed (see `infra/README.md`). The second label is the
escape hatch: if GitHub ever stops resolving a plain label that collides with a
scale-set name, heavy jobs can be pinned to this host explicitly without
touching the others.

### Step 3 -- state the machine's structural facts

Only if it has any. `infra/ansible/inventory/host_vars/bench-tower.yml`:

```yaml
---
ci_runner_docker_root: "/opt/ra8-ci-runner"
ci_runner_docker_dataset: ""            # no ZFS here
ci_runner_docker_forbidden_pools: []
ci_runner_docker_forbidden_mounts: []
```

### Step 4 -- prerequisites on the machine itself

The role deliberately does **not** install these, and fails loudly rather than
half-deploying:

- Docker Engine with the **Compose v2 plugin** (the standalone v1
  `docker-compose` silently ignores the resource caps, producing an uncapped
  runner the play would then report as capped),
- **cgroup v2** (what actually enforces the caps),
- the toolchain image archive `ra8-ci-runner.tar` under
  `<runner root>/images/`, copied from the k3s node. The image is never rebuilt
  per host: `.devcontainer/Dockerfile` is the single source of truth for every
  pinned tool, and the `toolchain-parity` gate exists to fail a runner that
  drifts from those pins, so a second independently-built image is a drift
  source with no upside.

### Step 5 -- converge it

```sh
make infra-check HOST=bench-tower                   # dry run first
gh api -X POST repos/bsikar/ra8-firmware/actions/runners/registration-token \
  --jq .token                                       # one hour, one use
make infra-apply HOST=bench-tower
```

Pass the token through if the host must not hold a long-lived PAT:

```sh
python3 scripts/dev/fleet.py apply bench-tower \
  -e ci_runner_docker_registration_token=<token>
```

Registration produces long-lived runner credentials **on the host**, and only
those are used to run jobs, so the token is needed once and never again -- not
even across a reboot.

> **`-e KEY=VALUE` is visible in `ps` on the control node.** That is tolerable
> for a registration or removal token -- one hour, one use, minted on demand --
> and it is not tolerable for a PAT. Ansible also reads `-e @file`, so a
> long-lived credential goes in a mode-0600 file outside the checkout:
>
> ```sh
> python3 scripts/dev/fleet.py apply bench-tower -e @~/.config/ra8/ci.yml
> ```
>
> The normal path is neither: the PAT lives in OpenBao and
> `infra/ansible/group_vars/all.yml` looks it up at run time.

### Step 6 -- verify

```sh
make infra-status
```

`tower-ci-1` and `tower-ci-2` should be `online`, and the per-host section
should show both containers `running`.

> **A dry run cannot tell you everything.** Ansible's `--check` mode skips the
> probe tasks the role's asserts read (Docker's version, the cgroup version,
> the pool backing each path), so those asserts see empty strings and fail. A
> clean `infra-check` is evidence of reachability and syntax; it is not a
> complete change list, and a failing one on a fresh host is usually this.

---

## 4. Retune a host

### Change the worker count

Edit one number:

```yaml
  win-ci:
    runners:
      instances: 1        # was 3
```

then:

```sh
make infra-apply HOST=win-ci
```

The apply **drains the host first**, because a converge recreates containers
and that would cancel their jobs. It then re-provisions and brings the new
count back up. Expect it to take as long as the longest job currently running
there.

**If you only want the change for now**, do not re-provision at all:

```sh
make infra-scale HOST=win-ci N=1
```

That parks the surplus instances as they go idle and leaves the rest running
untouched. Nothing is deregistered and nothing is rebuilt -- `make infra-scale
HOST=win-ci N=3` brings them straight back. This is the right tool for "I want
to play a game for an hour".

| | `infra-apply` | `infra-scale` |
|---|---|---|
| changes the declaration's meaning | yes -- it is the declaration | no |
| re-registers runners | yes | no |
| downtime for instances that stay | yes (all are drained) | none |
| survives a reboot | yes | yes (`docker stop` beats `restart: unless-stopped`) |
| undone by the next `infra-apply` | n/a | yes |

### Change the CPU or memory cap

```yaml
    runners:
      cpus: 6
      memory_gb: 8
```

Container caps are compose-file keys, so they need the container to be
recreated: `make infra-apply HOST=<host>`. Nothing takes effect live.

`pin_cpus: true` gives each instance its own cpuset rather than only a CFS
quota, and that is load-bearing rather than tuning: `cpus:` is a quota and
**`nproc` does not see it**, so every instance would report the host's full
thread count, every build would fan out `-j<all of them>`, and N instances
would oversubscribe the machine N-fold. A cpuset changes the affinity mask, so
`nproc` returns the instance's real share and `make -j$(nproc)` is right by
construction. Instance *i* gets threads `[(i-1)*cpus, i*cpus-1]`, so the host
needs at least `instances * cpus` threads.

### Change the WSL VM caps (`docker_wsl` only)

```yaml
    budget:
      threads: 16     # was 22
      memory_gb: 20   # was 26
```

These are written into the Windows user's `.wslconfig`, and they are the
**outer** limit: the per-instance container caps sit under them.

> **This one needs a restart.** `.wslconfig` is read when the WSL2 VM boots, so
> a change needs `wsl --shutdown` -- which stops **every** distro on that
> machine, not just the runner's. The tooling reports that and refuses to do it
> behind the owner's back. Run it yourself when the machine is free; the
> runners come back with the distro.
>
> Note also that `.wslconfig` is **per Windows user**. Written under the wrong
> profile it is silently ignored and every cap in it does nothing, which is why
> `connect.windows_user` is declared rather than guessed.

### What takes effect when

| change | effect |
|---|---|
| `make infra-scale HOST=x N=n` | live, drains first, no restart |
| `instances` | needs `infra-apply` (drains, re-registers) |
| `cpus`, `memory_gb`, `pin_cpus` | needs `infra-apply` (recreates containers) |
| `labels` | needs `infra-apply` (re-registration) |
| `quiet_hours` | `fleet.py apply <host> --tags capacity` -- no drain, no container touched |
| `budget.threads`, `budget.memory_gb` on `docker_wsl` | needs `infra-apply` **and** `wsl --shutdown` |
| `instances` on `arc_k8s` | `infra-apply`, or live with `infra-scale` |

---

## 5. Quiet hours

The motivating case: the fastest host in the fleet is also the owner's gaming
PC. It should stand CI down on request and come back afterwards, without anyone
remembering to do it.

```yaml
  win-ci:
    quiet_hours:
      window: "18:00-23:59"
      days: "Fri,Sat,Sun"
      instances: 0
```

Installing or changing a window does **not** need a full re-provision. The
capacity role is tagged, and that path touches no container:

```sh
python3 scripts/dev/fleet.py apply win-ci --tags capacity
```

`make infra-apply HOST=win-ci` installs it too, along with everything else. The
window is evaluated in the **host's own local time**, not UTC.

> That makes the host's clock load-bearing, and a WSL2 VM's clock is not
> reliable on its own -- it drifts and jumps across host sleep, which is
> visible in the drain logs as non-monotonic timestamps from a single process.
> The `wsl_ci_host` role waits for `timesyncd` to report `NTPSynchronized=yes`
> before it finishes for exactly this reason. If a window ever appears to fire
> at the wrong time, check the clock before the schedule.

### How it works, and why it is a poll rather than two alarms

The timer runs every 10 minutes and asks *"what should this host be right
now?"*. It does not fire at the window's edges.

Edge-triggered timers are wrong for a machine that might be switched off,
asleep or mid-upgrade at the moment one would have fired: the transition is
simply missed, and the host sits at the wrong capacity until the next edge --
which for a Friday-evening window means all weekend. Polling is level-triggered
and idempotent: a host that was off at 18:00 goes quiet at 18:10 instead, and a
host already at its target does nothing at all. It also removes the only
genuinely fiddly case, a window that crosses midnight, from the timer and puts
it in one place that can be reasoned about once.

Entering the window runs the same drain as `make infra-scale`: instances are
stopped as they go idle, never while they hold a job. Leaving it starts them
again.

### Verify it

```sh
# the schedule is installed and armed
ssh -J star sikar@10.0.40.100 \
  'wsl -d Ubuntu -u root -e systemctl list-timers ra8-fleet-capacity.timer'

# what it decided last time it ran
ssh -J star sikar@10.0.40.100 \
  'wsl -d Ubuntu -u root -e journalctl -u ra8-fleet-capacity.service -n 40'

# and what the host is actually running
make infra-status
```

The role asserts the timer is `active` after installing it: a schedule that is
written down and not running is exactly the kind of silent nothing this tree
keeps finding in its own tooling. What a healthy run looks like:

```
systemd[1]: Starting ra8-fleet-capacity.service - Apply the declared CI
            capacity window (drains, never kills)...
ra8-fleet-capacity[...]: outside   Fri,Sat,Sun 18:00-23:59: target 3
ra8-fleet-capacity[...]: resuming  ra8-ci-runner-1: was exited
ra8-fleet-capacity[...]: active    ra8-ci-runner-2: already running
ra8-fleet-capacity[...]: active    ra8-ci-runner-3: already running
systemd[1]: Finished ra8-fleet-capacity.service.
```

That run is also the level-triggered design paying for itself: the host was one
instance short of its declared capacity outside its window -- left parked by an
earlier manual scale -- and the timer simply put it back. An edge-triggered
pair of alarms would have had nothing to say until the next Friday.

### Deleting the block undoes it

Remove `quiet_hours:` and re-apply, and the role stops and deletes the units. A
window whose declaration is gone but whose timer still stands the host down
every Friday would be worse than never having had one.

### One host cannot have quiet hours

**TrueNAS SCALE mounts its appliance root read-only**, so `/etc/systemd/system`
cannot take a unit file. If a window is ever declared for it the role **fails**
rather than skipping. Manual `make infra-scale HOST=truenas N=<n>` works
normally there -- it pipes the capacity script over ssh and needs nothing
installed.

---

## 6. Remove a host

```sh
make infra-remove HOST=truenas
```

One command, and it is a real teardown: it drains the instances, stops the
containers, deregisters every runner from GitHub, removes the image and (on
TrueNAS) destroys the ZFS dataset, leaving nothing behind. Then delete the
host's block from `infra/fleet.yml` and its `host_vars` file.

Removal needs a credential, because deregistering is an API call. Either the
PAT, or a short-lived removal token for a host that must not hold one:

```sh
gh api -X POST repos/bsikar/ra8-firmware/actions/runners/remove-token --jq .token
python3 scripts/dev/fleet.py remove <host> -e ci_runner_docker_removal_token=<token>
```

`make infra-remove` refuses on a host whose roles do not all implement a
teardown path (the dev box, the k3s node, the bench). Only their roles own both
halves of the lifecycle; removing the others means undoing them by hand, which
is the drift the roles exist to prevent. Add a removal path to the role instead.

---

## 7. How capacity is decided

Instance count is **derived, not guessed**:

```
instances = min( budget.threads   / build_parallelism ,
                 budget.memory_gb / memory_per_instance_gb )
```

Both constants are measured properties of this tree, and both are named in
`infra/fleet.yml` so a new machine is sized by plugging in two numbers.

**`build_parallelism = 4`** -- every heavy workflow pins its own fan-out with
`CMAKE_BUILD_PARALLEL_LEVEL` (see `scripts/ci/lib/parallelism.sh`). A job
cannot use more CPUs than that no matter how many it is given, so CPU beyond it
per instance is bought and never spent.

**`memory_per_instance_gb = 8`** -- clang-tidy is the memory ceiling in this
tree and has been OOM-killed on an 8 GB machine. An instance that OOMs mid-job
is worse than one that never existed: it presents as a flaky gate, and the
diagnosis cost is out of all proportion to the capacity gained.

### It reproduces every hand-derived number in the fleet

| host | budget | formula | declared |
|---|---|---|---|
| `truenas` | 8 threads, 16 GB | `min(2, 2)` = **2** | 2 |
| `win-ci` | 22 threads, 26 GB | `min(5, 3)` = **3** | 3 |
| `k3s-pve` pod | 4 CPU, 12 GB limit | `min(1, 1)` = **1** per pod | 1 x 6 pods |

On the gaming PC, **memory is the binding constraint** and cores are not: 22
threads would divide fine at four instances, and `26/4 = 6.5` GB would land
back under the OOM threshold. Do not go to four until clang-tidy is sharded and
per-shard peak memory has been re-measured.

`truenas` ran **one** instance for its whole life on the same reserved budget,
which left half of a deliberately-sized reservation idle whenever one job was
all it could hold. It is two now, from the formula, with the host's total CI
footprint unchanged.

### The divisor is conservative, and revisable

Container memory caps are **ceilings, not reservations**, so the 8 GB divisor
protects the case where several heavy jobs coincide on one host -- it is not a
claim about one job's typical draw. Measured on `win-ci`: a full cross-build of
all 218 apps peaked at **4.06 GiB RSS**, and an idle runner listener holds
about **95 MiB**.

If per-job peak RSS is ever measured properly across the whole gate set and
stays well under the divisor, drop the divisor and every host's recommended
count rises with it. That is a measurement away, not a redesign. The number is
meant to be revised, not treated as magic.

### The gate keeps this honest

`check_fleet_declaration.py` recomputes the formula for every host and **fails**
when a declared count or per-instance cap departs from it with no written
`sizing_note`. Three hosts have real reasons to depart from it -- what the gate
enforces is that a departure is deliberate and legible. A number nobody can
re-derive is folklore.

### Why more machines rather than more pods on the first one

`k3s-pve` and the `dev` box are guests on the same 10-core / 16-thread
i5-12600K, together allocated 28 vCPU on a 16-thread part -- about 1.75x
oversubscribed before anything runs, and measured at load average 68-110 under
real use. Adding ARC pods there adds contention, not throughput. The same
`build-cross` gate, same commit, same toolchain image, same 8-CPU allocation:

| gate | `win-ci` | `truenas` | a `k3s-pve` pod |
|---|---|---|---|
| cross-build, all 218 apps | **258s** | 808s | 1689s |
| clang-tidy, full width | **96s** | ~330s | ~981s (contended) |

Real capacity comes from machines that are not `pve1`. That is what `truenas`
and the gaming PC are for, and why `ci_runner_max` stays at 6 as the fleet
grows.

---

## 8. What is not declarative, and why

- **Vault initialisation and unsealing.** Manual by design: both produce
  secrets, and a playbook that handles a root token can log one. See
  `scripts/secrets/README.md`.
- **`wsl --shutdown`.** Required for a `.wslconfig` change to take effect, and
  it stops every distro on someone's personal machine. The tooling reports that
  it is needed and leaves the decision with the owner.
- **The Proxmox guest topology.** VM 300 and CT 107 exist only as live guest
  config; recorded on issue #500.
- **Quiet hours on TrueNAS SCALE.** Blocked by a read-only appliance root, not
  by a missing feature -- see section 5.
- **A registration or removal token.** Minted per operation and passed on the
  command line, never stored. That is the point: two hosts in this fleet must
  never hold a long-lived credential.

---

## See also

- [`infra/fleet.yml`](../infra/fleet.yml) -- the declaration itself
- [`docs/INFRASTRUCTURE.md`](INFRASTRUCTURE.md) -- the whole estate, machine by machine
- [`infra/README.md`](../infra/README.md) -- per-role index and the runner-pool topology
- [`scripts/ci/fleet_capacity.sh`](../scripts/ci/fleet_capacity.sh) -- the drain, in full
- `make infra-help` -- the command surface
