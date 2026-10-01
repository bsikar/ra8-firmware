# The CI fleet: declared, not hand-built

Every machine this project's CI and HIL tooling touches is declared in **one
file**, [`infra/fleet.yml`](../infra/fleet.yml). The inventory, the playbook
selection, the transport and every role variable are derived from it.

The fleet is small and has no runner pool. `k3s-pve` is the single-node k3s
cluster that hosts the OpenBao vault (tools/ra8ci logs in to it with an AppRole
for its Terraform runs). The `dev` box carries the one native HIL listener, and
`star` is the bench Pi it drives. CI itself is `tools/ra8ci`; there are no
Actions workflows and no autoscaling runners.

```sh
just infra::ssh_config              turn THIS machine into a control node
just infra::list                    what is declared, and what each host runs
just infra::show k3s-pve            one machine in full, with its derived vars
just infra::status                  what every host is running, right now
just infra::check k3s-pve           DRY RUN -- report, change nothing
just infra::apply k3s-pve           converge that machine to the declaration
```

---

## 1. The declaration

```yaml
hosts:
  <name>:
    class: k8s_node | dev_box | hil_bench
    summary: "one line"
    connect:
      address: ci.example.net    # an IP or resolvable name -- NEVER an ssh alias
      user: ci-admin             # optional login account
      jump: bastion              # optional ProxyJump through ANOTHER FLEET HOST
    provisions: [play, play]     # from `just infra::list`
```

No host may declare `runners:`, `budget:`, `quiet_hours:` or `sizing_note:`;
`check_fleet_declaration.py` rejects them, because there is no runner pool for
them to describe.

### `connect` carries an ADDRESS, and that is what makes a control node

`connect.address` is an IP or a name a resolver can answer, never an
`~/.ssh/config` alias. Every ssh and Ansible invocation is built from
`address`, `user` and `jump`, so any machine with ansible and an accepted key
can drive the fleet. `just infra::ssh_config` GENERATES the friendly aliases
from the same fields.

### `class` picks the plays and the variable mapping

| class | what it is |
|---|---|
| `k8s_node` | the single-node k3s cluster that hosts the vault |
| `dev_box` | shared verification box + dedicated HIL listener |
| `hil_bench` | the hardware-in-the-loop bench Pi |

The classes live in `scripts/dev/fleet_runner_model.py`; the plays live in
`PLAYS` in `scripts/dev/fleet_model.py`.

### The native HIL listener

A `dev_box` may carry one `hil_runner:` block. It owns the repo-scoped
registration identity, custom labels, and relationship to a declared
`hil_bench` host:

```yaml
hil_runner:
  name: <repository runner name>
  repository: https://github.com/<owner>/<repository>
  labels: [<custom-label>, <board-label>]
  bench:
    host: <hil-bench fleet host>
    aliases: [<other accepted spelling>]
```

`fleet_model.role_vars()` resolves the bench's declared address and derives
the `dev_box_hil_runner_*` inputs consumed by the Ansible role. The role's
identity/topology defaults are intentionally empty so a direct playbook run
cannot recreate a second declaration by accident. First registration is the
sole converge that needs an ephemeral GitHub registration token; follow the
mode-0600 procedure in [`infra/README.md`](../infra/README.md#the-dev-box).

### What is NOT in the declaration

Structural facts about a machine live in
`infra/ansible/inventory/host_vars/<host>.yml`. Those are properties of the
machine, not knobs. `check_fleet_declaration.py` fails a `host_vars` file that
re-declares any variable the declaration owns: extra-vars beat `host_vars`, so
a duplicate would look authoritative and do nothing.

---

## 2. Add or retune a host

1. Add or edit its block in `infra/fleet.yml`.
2. `python3 scripts/checks/check_fleet_declaration.py` and `just infra::list`.
3. `just infra::check <host>`, read the diff, then `just infra::apply <host>`.

---

## 3. What is not declarative, and why

- **Vault initialisation and unsealing.** Manual by design: both produce
  secrets, and a playbook that handles a root token can log one. See
  `scripts/secrets/README.md`.
- **The Proxmox guest topology.** VM 300 and CT 107 exist only as live guest
  config; recorded on issue #500.
- **The HIL listener's registration token.** Minted once for first
  registration and passed in a mode-0600 vars file, never stored.
- **Which keys a host authorises.** The declaration says where a machine is and
  which account you enter it as; it deliberately does not carry key material.
  `just infra::doctor` reports a refused key as `MISS`; adding one is an
  `ssh-copy-id` from a machine that already has access.

---

## See also

- [`infra/fleet.yml`](../infra/fleet.yml) -- the declaration itself
- [`docs/INFRASTRUCTURE.md`](INFRASTRUCTURE.md) -- the whole estate, machine by machine
- [`infra/README.md`](../infra/README.md) -- per-role index
- `just infra` -- the command surface
