# The CI fleet: declared, tuned, and scheduled

Every machine that runs CI for this project is declared in **one file**,
[`infra/fleet.yml`](../infra/fleet.yml). Changing how much of a machine CI is
allowed to use is changing a number in that block. Nothing else has to be
edited: the inventory, the playbook selection, the transport and every role
variable are derived from it.

The fleet is small now. General CI capacity is one ARC scale set on the k3s
node (`k3s-pve`). The `dev` box carries the one native HIL listener, and `star`
is the bench Pi it drives. The plain Docker and WSL runner hosts are gone.

```sh
just infra::ssh_config              turn THIS machine into a control node
just infra::list                    what is declared, and how it is sized
just infra::show k3s-pve            one machine in full, with its derived vars
just infra::status                  what every host is running, right now
just infra::check k3s-pve           DRY RUN -- report, change nothing
just infra::apply k3s-pve           converge that machine to the declaration
just infra::scale k3s-pve 3         live capacity change
```

---

## 1. Scaling down never kills a job

ARC runners are ephemeral: one job, then the pod exits, and the controller only
deletes runners that hold no job. So lowering the scale set's `maxRunners`
never interrupts work. Idle runners are retired down to the new ceiling and
running jobs finish first.

[`scripts/ci/fleet_capacity.sh`](../scripts/ci/fleet_capacity.sh) moves that
number with `kubectl patch ... --field-manager=helm`, so the live change and
the next Helm upgrade share field ownership instead of fighting over it.

---

## 2. The declaration

```yaml
sizing:
  build_parallelism: 4        # CMAKE_BUILD_PARALLEL_LEVEL
  memory_per_instance_gb: 8

hosts:
  <name>:
    class: arc_k8s | dev_box | hil_bench
    summary: "one line"
    connect:
      address: ci.example.net    # an IP or resolvable name -- NEVER an ssh alias
      user: ci-admin             # optional login account
      jump: bastion              # optional ProxyJump through ANOTHER FLEET HOST
    provisions: [play, play]     # from `just infra::list`
    runners:                     # arc_k8s only
      instances: 6               # the scale set's maxRunners
      cpus: 4                    # per-pod limit
      memory_gb: 12              # per-pod limit
      cpu_request: 1
      memory_request_gb: 2
      labels: [ra8-ci]           # the first label is the scale-set name
    budget:                      # what CI may use OF the machine
      mode: burst
      threads: 9
      memory_gb: 40
    quiet_hours:                 # optional
      window: "18:00-23:59"
      days: "Fri,Sat,Sun"
      instances: 0
    sizing_note: >-              # required if the count departs from the formula
      why this host is not sized by the formula
```

### `connect` carries an ADDRESS, and that is what makes a control node

`connect.address` is an IP or a name a resolver can answer. It is **never** an
`~/.ssh/config` alias, and `check_fleet_declaration.py` rejects a bare label
outright.

Aliases are local convenience configuration and are not portable between
control nodes. A literal address or resolvable name keeps both Ansible and the
fleet tooling independent of one maintainer's SSH configuration.

Every runner host asks the control node to resolve it.

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
just infra::ssh_config
```

That writes `~/.ssh/ra8-fleet.config` from the declaration and puts one
`Include` line at the top of `~/.ssh/config`. Top, because ssh takes the
**first** value it obtains for each keyword: the declaration wins over a stale
hand-written alias of the same name, while anything the fragment does not set
(your `IdentityFile`, a `Port`) still comes from your own block below it.
`just infra::setup` runs it as part of onboarding.

**Do not hand-write the aliases into a control node's `~/.ssh/config`.** That
is the same per-machine prerequisite one level down, and it rots the same way.
Print what would be installed with `just infra::ssh_config_preview`.

The fragment is a **convenience, not a dependency**: it exists so `ssh k3s-pve`
works for a person and for the scripts and docs that already spell hosts that
way. Nothing in `fleet.py` reads it, which is why a machine with an empty
`~/.ssh/config` can still drive the whole fleet.

Two things a fresh control node does still need, and neither is a name:

1. **ansible** -- run `just setup_ansible`; uv installs locked
   `ansible-core` and the recipe installs exact repository-local Galaxy
   collections.
2. **a key each host accepts** for its declared `connect.user`. `just
   infra::doctor` reports a host your key is not authorised on as `MISS`; that
   is an authorisation gap, not a resolution one.

Host keys are not a third prerequisite: every fleet ssh command carries
`StrictHostKeyChecking=accept-new`, which pins a key on first use and still
refuses one that later *changes*. That is strictly stronger than the fleet's
Ansible transport, which sets `host_key_checking = False`.

### `class` picks the arithmetic, the plays and the variable mapping

| class | what it is | capacity is changed by |
|---|---|---|
| `arc_k8s` | an ARC scale set on a k8s cluster | patching `maxRunners` |
| `dev_box` | shared verification box + dedicated HIL listener | n/a (not general capacity) |
| `hil_bench` | the hardware-in-the-loop bench Pi (not a runner) | n/a |

### A native HIL listener is declared, but is not scalable capacity

A `dev_box` may carry one `hil_runner:` block. It owns the repo-scoped
registration identity, custom labels, Actions workflow, and relationship to a
declared `hil_bench` host:

```yaml
hil_runner:
  name: <repository runner name>
  repository: https://github.com/<owner>/<repository>
  labels: [<custom-label>, <board-label>]
  workflow: .github/workflows/<workflow>.yml
  bench:
    host: <hil-bench fleet host>
    aliases: [<other accepted spelling>]
```

`fleet_model.role_vars()` resolves the bench's declared address and derives
the `dev_box_hil_runner_*` inputs consumed by the Ansible role. The role's
identity/topology defaults are intentionally empty so a direct playbook run
cannot recreate a second declaration by accident.

The fleet checker validates the relationship in both directions: every job in
the owned workflow must request exactly `self-hosted` plus the declared custom
labels, and no unowned workflow may use those labels. The listener remains
outside `runners:`/`budget:` because it has no instance count or scale
operation. First registration is the sole converge that needs an ephemeral
GitHub registration token; follow the mode-0600 procedure in
[`infra/README.md`](../infra/README.md#the-dev-box).

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

## 3. Retune the scale set

Edit the number in `infra/fleet.yml` and converge:

```yaml
  k3s-pve:
    runners:
      instances: 4        # was 6
```

```sh
just infra::apply k3s-pve "" ci-runner
```

For a change that should only last a while, skip the converge:

```sh
just infra::scale k3s-pve 2
```

That patches `maxRunners` live, and it is temporary: the capacity timer
(section 4) puts the declared number back within ten minutes. To stand the
pool down durably, change `instances:` or add a `quiet_hours` block.

| change | effect |
|---|---|
| `just infra::scale k3s-pve n` | live, idle runners retire, no restart |
| `instances` | `infra::apply` (or live with `infra::scale`) |
| `cpus`, `memory_gb`, requests | `infra::apply` (new pods get the new limits) |
| `labels` | `infra::apply` |
| `quiet_hours` | `just infra::apply k3s-pve "" capacity` |

Adding real capacity means another machine, not more pods on this one (see
the `sizing_note` on `k3s-pve`). A new `arc_k8s` host is declared the same way
and converged with `just infra::check <host>` then `just infra::apply <host>`.

---

## 4. Quiet hours

A `quiet_hours` block lowers the scale set to `instances` inside the window and
puts it back outside it. Installing or changing one touches nothing else:

```sh
just infra::apply k3s-pve "" capacity
```

The window is evaluated in the host's own local time. A systemd timer runs
every 10 minutes and asks what the host should be right now, rather than
firing at the window's edges: a machine that was off or mid-upgrade at the
edge still converges on the next tick, and a window that crosses midnight is
handled in one place.

Every runner host carries the timer, window or not. Without a window it
converges the host to its declared `instances`, which is what makes a live
`just infra::scale` temporary and what heals a scale-down someone forgot to
undo. Deleting the block therefore just leaves the declared count in force.
`/etc/systemd/system` has to be writable; the role fails on a host where it is
not rather than converge one whose capacity nothing re-asserts.

---

## 5. How capacity is decided

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

### The gate keeps this honest

`check_fleet_declaration.py` recomputes the formula for every host and **fails**
when a declared count or per-instance cap departs from it with no written
`sizing_note`. Some hosts have real reasons to depart from it -- what the gate
enforces is that a departure is deliberate and legible. A number nobody can
re-derive is folklore.

---

## 6. What is not declarative, and why

- **Vault initialisation and unsealing.** Manual by design: both produce
  secrets, and a playbook that handles a root token can log one. See
  `scripts/secrets/README.md`.
- **The Proxmox guest topology.** VM 300 and CT 107 exist only as live guest
  config; recorded on issue #500.
- **The HIL listener's registration token.** Minted once for first
  registration and passed in a mode-0600 vars file, never stored.
- **Which keys a host authorises.** The declaration says where a machine is and
  which account you enter it as; it deliberately does not carry key material or
  an `authorized_keys` list. So a fresh control node can *resolve and reach*
  every host the moment it runs `just infra::ssh_config`, and still be refused
  by one whose `authorized_keys` it is not in -- `just infra::doctor` reports
  that as `MISS`. Adding a key is a one-line `ssh-copy-id` from a machine that
  already has access, not a fleet change.

---

## See also

- [`infra/fleet.yml`](../infra/fleet.yml) -- the declaration itself
- [`docs/INFRASTRUCTURE.md`](INFRASTRUCTURE.md) -- the whole estate, machine by machine
- [`infra/README.md`](../infra/README.md) -- per-role index and the runner-pool topology
- [`scripts/ci/fleet_capacity.sh`](../scripts/ci/fleet_capacity.sh) -- the drain, in full
- `just infra` -- the command surface
