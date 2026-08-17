# The CI fleet: declared, tuned, and scheduled

Every machine that runs CI for this project is declared in **one file**,
[`infra/fleet.yml`](../infra/fleet.yml). Adding a machine is adding a block to
it. Changing how much of a machine CI is allowed to use is changing a number in
that block. Nothing else has to be edited: the inventory, the playbook
selection, the transport and every role variable are derived from it.

This document is the runbook for the five things you will actually want to do:
add a host, retune one, give one quiet hours, remove one, and lend one back to
agents as a verification host (section 9). Section 7 explains how the numbers
were arrived at, so a new machine can be sized without re-deriving anything.

```sh
make infra-ssh-config              make THIS machine a control node
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
      address: 10.10.10.1        # an IP or a resolvable name -- NEVER an ssh alias
      user: truenas_admin        # optional login account
      jump: star                 # optional ProxyJump through ANOTHER FLEET HOST
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
    dev_slice:                   # optional; docker classes only -- section 9
      cpu_weight: 10             # against 100 for CI
      memory_gb: 5               # HARD cap, carved out of budget.memory_gb
      swap_gb: 3
      max_jobs: 6
    sizing_note: >-              # required if the count departs from the formula
      why this host is not sized by the formula
```

### `connect` carries an ADDRESS, and that is what makes a control node

`connect.address` is an IP or a name a resolver can answer. It is **never** an
`~/.ssh/config` alias, and `check_fleet_declaration.py` rejects a bare label
outright.

That rule was bought with real work. Every host used to be addressed as
`ssh: truenas` / `ssh: star` / `ssh: k3s-pve`, which resolve only through one
machine's private config. The estate then split so that neither half worked:

| | ansible | fleet host aliases |
|---|---|---|
| the Mac | no | yes |
| the dev box | yes | no |

So `fleet.py status truenas` from the dev box died on
`Could not resolve hostname truenas` while the machine itself answered fine on
`10.10.10.1`. It was a naming gap, not a routing one -- and it cost a NAS that
sat at 1 of its declared 2 runners with nothing able to converge it back, plus
a runner-image rollout done by hand on all three hosts (#513, #518, #526).

`win-ci` hid it: it is `docker_wsl`, so `fleet.py` ships the play *into* the
distro and runs it `--connection=local`, never asking the control node to
resolve anything. Every `docker_linux` and `k8s` host does ask.

**Everything is derived from the address now**, so no command in this tooling
needs a name your machine happens to know:

- `fleet.py` builds each `ssh` argv as
  `ssh -J <hop address> <user>@<address>` -- literals only,
- the generated inventory sets `ansible_host` / `ansible_user` and, for a
  jumped host, `ansible_ssh_common_args='-o ProxyJump=...'`,
- `connect.jump` names **another host in this file**, so a bastion's address is
  declared once and resolved exactly the way its target is,
- `scripts/dev/infra.sh` asks `fleet.py ssh-target <host>` rather than spelling
  a host anywhere.

The gate checks both ends: the declared address must be a literal, *and* every
destination the model derives -- ssh argv and inventory line alike -- must be
one too. A future `-J star` would pass every input rule and still only work
where that alias existed.

#### Make this machine a control node

```sh
make infra-ssh-config       # or: python3 scripts/dev/fleet.py ssh-config --install
```

That writes `~/.ssh/ra8-fleet.config` from the declaration and puts one
`Include` line at the top of `~/.ssh/config`. Top, because ssh takes the
**first** value it obtains for each keyword: the declaration wins over a stale
hand-written alias of the same name, while anything the fragment does not set
(your `IdentityFile`, a `Port`) still comes from your own block below it.
`make infra-setup` runs it as part of onboarding.

**Do not hand-write the aliases into a control node's `~/.ssh/config`.** That
is the same per-machine prerequisite one level down, and it rots the same way.
Print what would be installed with `python3 scripts/dev/fleet.py ssh-config`.

The fragment is a **convenience, not a dependency**: it exists so `ssh truenas`
works for a person and for the scripts and docs that already spell hosts that
way. Nothing in `fleet.py` reads it, which is why a machine with an empty
`~/.ssh/config` can still drive the whole fleet.

Two things a fresh control node does still need, and neither is a name:

1. **ansible** -- `pipx install ansible-core`, or `brew install ansible`.
2. **a key each host accepts** for its declared `connect.user`. `make
   infra-doctor` reports a host your key is not authorised on as `MISS`; that
   is an authorisation gap, not a resolution one.

Host keys are not a third prerequisite: every fleet ssh command carries
`StrictHostKeyChecking=accept-new`, which pins a key on first use and still
refuses one that later *changes*. That is strictly stronger than the fleet's
Ansible transport, which sets `host_key_checking = False`.

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
Ryzen 7 5800X with 16 threads and 32 GB, running Ubuntu, at `192.168.1.40` as
the `deploy` user, that should give CI half of itself.

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
      address: 192.168.1.40
      user: deploy
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
| `dev_slice` | `fleet.py apply <host> --tags dev-slice` -- no drain, no container touched |
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

A host that also lends a **dev slice** (section 9) has that frozen by the same
run, because parking three idle containers while an agent's gate suite goes on
using the machine would defeat the whole window. The rule is one line: *the dev
slice is frozen exactly while the host's runner target is zero* -- so both the
timer and a manual `make infra-scale HOST=win-ci N=0` stand down all of it, and
both thaw it again the moment the target is non-zero. Note the consequence of
the timer converging capacity generally: a manual `N=0` is undone within ten
minutes, and the dev slice thaws with it. A durable stand-down is a
`quiet_hours` block or an `instances:` change, exactly as it is for the
runners.

### Verify it

```sh
# the schedule is installed and armed
ssh win-ci 'wsl -d Ubuntu -u root -e systemctl list-timers ra8-fleet-capacity.timer'

# what it decided last time it ran
ssh win-ci 'wsl -d Ubuntu -u root -e journalctl -u ra8-fleet-capacity.service -n 40'

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

Remove `quiet_hours:` and re-apply, and the host converges to its declared
instance count at all times -- the timer stays, because the window was only ever
one input to it.

### The timer is not only for quiet hours

**Every runner host carries it**, window or not, and that is a fix rather than a
generalisation. `make infra-scale HOST=truenas N=1` drained `ra8-ci-runner-2`
during a bench session; `restart: unless-stopped` deliberately does not undo an
explicit stop; truenas declares no window, so under the old shape it had no
timer; so nothing ever re-asked what the host should be. The NAS served CI at
half its declared capacity for hours, and the only thing in the tree that knew
was `make infra-status`, which prints the drift and exits 0. win-ci, which does
declare a window, healed the identical fault every ten minutes without anyone
noticing there had been one.

So `fleet_capacity.sh window` answers *"what should this host be right now"* on
a host with no window too: its declared capacity. The consequence is
deliberate -- **a live `make infra-scale` is temporary**. To stand a host down
durably, change `instances:` in `infra/fleet.yml` (or give it a `quiet_hours`
block) and re-converge. That is a capacity decision the next person can find;
a parked container on a machine nobody is looking at is not.

### A host that cannot hold the timer is refused

`/etc/systemd/system` has to be writable, and the role **fails** on a host where
it is not rather than converging a machine whose capacity nothing re-asserts.
The case it was written for is an appliance with a read-only root: TrueNAS SCALE
mounts `/` `ro`. On the SCALE release the NAS runs, `/etc/systemd/system` is
writable and it does hold the timer -- which is why the role measures the
directory instead of inferring it from the distribution. Manual
`make infra-scale HOST=truenas N=<n>` works either way: it pipes the capacity
script over ssh and needs nothing installed.

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
all apps peaked at **4.06 GiB RSS** (218 apps at the time of measurement;
the tree grows, the shape does not), and an idle runner listener holds
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
| cross-build, all apps (218 when measured) | **258s** | 808s | 1689s |
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
- **Which keys a host authorises.** The declaration says where a machine is and
  which account you enter it as; it deliberately does not carry key material or
  an `authorized_keys` list. So a fresh control node can *resolve and reach*
  every host the moment it runs `make infra-ssh-config`, and still be refused
  by one whose `authorized_keys` it is not in -- `make infra-doctor` reports
  that as `MISS`. Adding a key is a one-line `ssh-copy-id` from a machine that
  already has access, not a fleet change.

---

## 9. Lending a runner host back: the dev slice

`win-ci` is the fleet's fastest machine and it is idle most of the time --
measured at load ~2.8 of 22 threads in the troughs between CI bursts. A
**dev slice** hands those troughs to agents: a workspace and a gate run on the
same box the runners are on, without CI ever losing a scheduling contest.

```yaml
  win-ci:
    dev_slice:
      cpu_weight: 10     # against 100 for CI
      memory_gb: 5       # HARD, and carved out of budget.memory_gb
      swap_gb: 3
      max_jobs: 6
```

```sh
python3 scripts/dev/fleet.py apply win-ci --tags dev-slice   # no drain
```

Delete the block and re-apply, and the slice, its entry point, its shell
environment and its reaper are removed. That is also how the 5 GiB goes back to
the Ethos-U55 / NPU work (#228) when that starts: it is one block, in one file.

### Why a weight and a wall, and not a share of the machine

The two resources behave completely differently under contention, so they are
declared differently.

**CPU is a weight.** Every runner container is a scope under `system.slice`,
which carries systemd's default `CPUWeight=100`. The dev slice is that slice's
sibling at 10. On a thread a runner wants, dev work gets 1/11th of it; on a
thread no runner wants, it gets all of it; and it yields within one scheduling
period of a job arriving, with nothing to schedule and nobody to remember
anything. A cpuset or a `--cpus` quota would have been exactly backwards --
idle while CI is busy, and capped while CI is idle.

**Memory is a hard wall,** because memory does not yield: a page a dev build
holds is a page a `clang-tidy` job cannot have. `MemoryMax` is therefore taken
out of the host's budget *before* the runners are sized, and
`check_fleet_declaration.py` fails a declaration where
`instances * memory_gb + dev_slice.memory_gb` overruns `budget.memory_gb`. On
`win-ci` the arithmetic is closed: 3 x 7 + 5 = 26, the VM cap exactly.

**Swap is allowed here and nowhere else in the fleet.** A runner that swaps is
a job that has silently become an order of magnitude slower and presents as a
timeout; a dev gate run that swaps is just slow, and slow beats an OOM kill.

**I/O is deliberately not declared.** The WSL2 kernel exposes no `io.weight`
(it builds neither BFQ nor blk-iocost), so an `IOWeight=` would be
configuration that does nothing. Stated here so nobody re-derives it.

### The one systemd trap in this, and it is silent

The unit is `ra8dev.slice` -- **no dash**. systemd reads `-` in a slice name as
*hierarchy*: `ra8-dev.slice` is created as a child of an auto-generated
`ra8.slice`, and it is `ra8.slice`, at the default weight of 100, that would
then face `system.slice`. The careful `CPUWeight=10` would be arbitrating
between the dev slice and its zero siblings while CI and dev split the machine
evenly one level up. Verified on the host: `systemd-run --slice=ra8-dev.slice`
lands in `/ra8.slice/ra8-dev.slice/...`. The role asserts the name is a single
token, and the model and the role are cross-checked against each other by the
`fleet-declaration` gate.

### Using it (this is the part a person needs)

```sh
ssh win-ci                                    # into Windows (make infra-ssh-config)
wsl -d Ubuntu                                 # into the distro (root)

make ws-new NAME=my-task                      # /opt/ra8-dev/ws/my-task
cd /opt/ra8-dev/ws/my-task

ra8-dev make ci                               # every gate, in the slice
ra8-dev make ci-fast                          # ...minus the slow ones
ra8-dev make ci-gate-container GATE=tidy      # exactly one gate
make ws-free NAME=my-task
```

`ra8-dev` is the whole interface. Everything else -- the workspace root, the
shared ccache, the bounded `-j`, and the `--cgroup-parent` that puts the gate
container in the slice -- is exported from `/etc/profile.d/ra8-dev-slice.sh`
and needs no thought.

> **`ra8-dev`, not bare `make ci`.** Two kinds of work have to land in the
> slice and they get there differently. Host-side work (`make`, `git`, a cross
> build) is *moved* there by `ra8-dev`, which starts it in a transient scope in
> the slice -- a login shell is otherwise in `user.slice`, beside CI rather
> than under it. The gate *container* cannot be moved that way at all: the
> docker daemon creates its cgroup, not your shell, so it inherits nothing.
> That half is done by `RA8_CI_CONTAINER_ARGS=--cgroup-parent=ra8dev.slice`,
> which `scripts/ci.sh` appends to its `run` command. A bare `make ci` from a
> login shell therefore still caps the container correctly, but its `make` and
> `git` run outside the slice. Use `ra8-dev`.

### Why the gates run in a container here and natively on the dev box

The dev box carries the pinned toolchain natively, because it is not a runner
and nothing else on it defines one. `win-ci` already holds the exact image its
own runners boot -- `.devcontainer/Dockerfile` plus the actions-runner layer --
so a second, apt-installed toolchain beside it would be a drift source with no
upside, and the `ci_runner_docker` role refuses to build a second image for
precisely that reason. The dev slice therefore reuses the runners' image: the
role tags it `ra8-ci:latest` (the name `scripts/ci.sh` boots) and **asserts
both names resolve to one image id**, so "a gate run in the dev slice proves
something about the runners beside it" is a checked fact.

The consequence is that `make ci-gate GATE=x` -- which runs a gate *natively*,
because that is what a CI runner does -- does not work in this distro. Use
`make ci-gate-container GATE=x`, which runs the same gate on the same clean
snapshot of committed `HEAD` inside that image. (It works on macOS too, where
`make ci-gate` has never been able to.)

### Quiet hours freeze it, they do not kill it

The capacity script freezes the slice whenever the host's runner target is 0 --
inside a declared window, or after a manual `make infra-scale HOST=x N=0`.
`systemctl freeze` suspends every process in it -- including the gate
container, whose scope is a child of the slice -- and thawing resumes them
exactly where they stood. An agent's suite pauses for the evening instead of
dying at 18:00, which is the same "never destroy work in flight" rule the
runner drain follows.

Because the capacity timer converges every host to its declared capacity every
ten minutes, a manual `N=0` freeze is temporary: the next poll restores the
runners and thaws the slice. Standing the machine down for an evening is a
`quiet_hours` window, not a manual scale.

The cost, stated rather than hidden: a run frozen for hours resumes with its
wall-clock budgets already spent, so a time-budgeted gate can fail on the way
out. That is why `ra8-dev` **refuses to start new work while the slice is
frozen** and says how to check. A paused run is the caller's informed choice; a
run that silently began five minutes before a window is not.

`make infra-status` reports the slice beside the runners:

```
win-ci (docker_wsl, declared 3 instance(s)):
  INSTANCE             STATE     BUSY  DETAIL
  ra8-ci-runner-1      running   idle  ready to park
  ra8-ci-runner-2      running   idle  ready to park
  ra8-ci-runner-3      running   idle  ready to park
  ra8dev.slice         active    running dev work runs at its weight, below CI
```

### What it cost CI: measured, not asserted

The whole point of a weight is that CI does not notice. That was tested rather
than assumed.

**The method.** A probe container given a **runner's exact caps** -- cpuset
14-20, 7 CPU, 7 GiB, `--pids-limit 8192`, and the default `system.slice`
parent, i.e. indistinguishable from `ra8-ci-runner-3` -- runs three real gates
on a clean snapshot of committed `HEAD`. Identical work, identical caps,
identical cores; the *only* difference between the phases is whether a full
`ra8-dev make ci` is running in the slice at the same time.

Nothing was parked to make room. Two earlier attempts drained runner 3 so the
probe owned its cpuset, and the fleet's own capacity timer put it straight back
within ten minutes -- correctly, which is now the documented behaviour
(section 4). So the *other* contention is **measured** instead: a sampler reads
every runner's busy state every 5 seconds for the whole round, and a round in
which runner 3 was ever busy is reported CONTAMINATED rather than quietly
averaged in. Only clean rounds are compared below.

| gate | dev slice idle | dev slice loaded | median change | observed range |
|---|---|---|---|---|
| `unit-tests` | 72.6 s | 77.0 s | **+6.0%** | +3.2% .. +16.8% |
| `misra` | 107.0 s | 117.8 s | **+10.1%** | +4.3% .. +16.4% |
| `tidy` | 202.2 s | 211.3 s | **+4.5%** | +1.7% .. +8.7% |

4 clean control rounds, 7 clean loaded rounds. The absolute numbers are a
cold-build worst case and are NOT comparable with the same gates' durations in
the Actions history -- a real runner works incrementally in its own `_work`
tree, the probe rebuilds from a fresh snapshot every time. The comparison
between the two columns is the measurement.

**Flakiness: none.** 33 of 33 gate runs across every round exited 0, in both
phases. The slice sat *at* its `MemoryMax` for much of the loaded phase
(98405 `memory.max` events in one 20-minute run) with `oom_kill 0` -- the hard
cap plus swap turned the peak into a slow dev run rather than a dead one, and
CI never saw it.

**The residual cost is not the scheduler, and cannot be tuned away.** Two
alternatives were measured on the same rig:

- **`cpu.idle=1` (SCHED_IDLE)** on the slice, which is a strictly stronger
  yield than any weight -- the cgroup runs only when nothing else wants the
  CPU. Result: 75.0 / 112.2 / 205.7 against 75.7 / 111.6 / 206.1 at
  `CPUWeight=10`. No measurable difference.
- **`max_jobs: 3`** instead of 6, halving the number of dev threads competing
  for cores. Result: 75.4 / 112.9 / 209.2. Again no measurable difference.

Both point the same way: the weight has already reduced the *runqueue* share to
near nothing, and what is left is SMT siblings and shared last-level cache --
physics of two workloads on one die, which no cgroup knob reaches. Removing it
would mean giving the slice cores no runner uses, and this host has exactly one
spare thread. So `cpu.idle` was not adopted (it buys nothing and its setting
does not survive a slice restart) and `max_jobs` stays at 6, where the smaller
number would have cost dev half its throughput for no gain to CI.

**For scale, the fleet already pays more than this to itself.** In the same
run, the contaminated control rounds -- dev slice *idle*, a real CI job on
runner 3 sharing the probe's cpuset -- came in at +0.3%..+10.1%,
+4.4%..+18.0% and +2.7%..+13.6% on the same three gates. Co-tenancy with the
dev slice costs a CI job about what co-tenancy with another runner already
costs it on this machine, and that is a trade the fleet made deliberately when
it put three runners on one die.

The harness is in issue #519.

---

## See also

- [`infra/fleet.yml`](../infra/fleet.yml) -- the declaration itself
- [`docs/INFRASTRUCTURE.md`](INFRASTRUCTURE.md) -- the whole estate, machine by machine
- [`infra/README.md`](../infra/README.md) -- per-role index and the runner-pool topology
- [`scripts/ci/fleet_capacity.sh`](../scripts/ci/fleet_capacity.sh) -- the drain, in full
- `make infra-help` -- the command surface
