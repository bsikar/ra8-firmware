# The estate

Every machine this project runs on: what it is, what it does, how it relates to
the others, and how to rebuild it from nothing.

This is the document to read when you have forgotten how any of it works. It is
deliberately narrative rather than a role-by-role reference -- the roles
document themselves, and `infra/README.md` is the per-role index.

**One command orients you:**

```sh
just infra::status     # what is deployed across the estate, right now (read-only)
just infra::list       # what machines are declared, and how they are sized
just infra::doctor     # can THIS machine drive any of it?
```

**The machines themselves are declared in `infra/fleet.yml`** -- one block per
host, and everything downstream is derived from it. This document is the
narrative; [`CI_FLEET.md`](CI_FLEET.md) is the runbook for *changing* the
fleet: adding a host, retuning one, giving one quiet hours, removing one, and
how instance counts are derived rather than guessed.

---

## 1. The shape of it

```
                         ,-- the one physical box everything heavy sits on --.
                        |                                                      |
   +--------------------+-------------------------------------------------+   |
   |  pve1   Proxmox 8.4   i5-12600K -- 10 cores / 16 THREADS, 125 GB RAM  |   |
   |                                                                       |   |
   |   +---------------------------+   +-------------------------------+   |   |
   |   | VM 300  "k3s"             |   | CT 107  "dev"                 |   |   |
   |   |  16 vCPU / 64 GB / 500 GB |   |  12 vCPU / 24 GB / 360 GB     |   |   |
   |   |  Ubuntu 24.04             |   |  Debian 12 (unprivileged LXC) |   |   |
   |   |                           |   |                               |   |   |
   |   |  - k3s (single node)      |   |  - every agent's `just ci`    |   |   |
   |   |  - ARC runner pool        |   |  - the pinned host toolchain  |   |   |
   |   |  - OpenBao vault          |   |  - ~/ra8-ws agent workspaces  |   |   |
   |   |  - the homelab (see 6)    |   |  - shared ccache              |   |   |
   |   +---------------------------+   +-------------------------------+   |   |
   |            16 vCPU        +        12 vCPU     =  28 vCPU  on 16      |   |
   +-----------------------------------------------------------------------+   |
                                                                               |
   Independent machines (NOT on pve1):                                         |
                                                                               |
   truenas ....... NAS + two CI runners in Docker                              |
   gaming PC ..... Ryzen 9 7900X, Windows + WSL2, three CI runners             |
   star .......... Raspberry Pi -- the HIL bench (board, J-Link, C6, AD2)      |
   FortiGate+AP .. odd 10.0.40.0/24 islanded; even 10.0.41.0/24 uplinked      |
```

### The single most load-bearing fact

**`k3s-pve` and the `dev` box are both guests on the same 10-core / 16-thread
i5-12600K, and together they are allocated 28 vCPU on a 16-thread part.**

That is roughly 1.75x oversubscribed before anything runs. In practice it is far
worse under load: the k3s node alone has been measured at load average 68-110
against its own 16 vCPU while agents were simultaneously running gates in the
`dev` box guest beside it.

This is *the* reason CI feels slow, and almost every "the runner timed out"
investigation eventually lands back here. Two consequences follow, and both are
already encoded in the roles rather than left as folklore:

- **Adding ARC pods does not add throughput.** The pod ceiling is deliberately
  lower than the thread count would suggest. The reasoning -- with the measured
  numbers -- is written at length in
  `infra/ansible/roles/ci_runner/defaults/main.yml`. Read it before raising
  that value; the answer is almost certainly "no".
- **Real capacity comes from machines that are not pve1.** That is exactly what
  `truenas` and the gaming PC are for. Both answer the same `ra8-ci` label, so
  GitHub spreads `runs-on: ra8-ci` across three independent machines instead of
  piling it onto the one that is already oversubscribed at the hypervisor level.

If pve1's headroom ever genuinely improves, the number to re-check first is
total vCPU commitment on the physical host -- not any single guest's setting.

---

## 2. The machines, one at a time

### `pve1` -- the hypervisor

Proxmox. `ssh pve`, user `pve-admin`, passwordless sudo. Bridges:
`vmbr0` 192.168.1.50/24 (LAN) and `vmbr1` 10.10.10.2/29 (a 2.5 Gb link to
TrueNAS, MTU 9000). Storage is `local` (dir) plus `local-lvm` (lvmthin, ~1.8 TB).

**Not codified.** The guest definitions exist only as live config. This is the
one remaining hole in the rebuild story -- see section 5.

### VM 300 `k3s` -- CI cluster and vault

16 vCPU, 64 GB, 500 GB, Ubuntu 24.04, two PCIe devices passed through. Reachable
as `ssh k3s-pve` from any control node: `infra/fleet.yml` declares its tailnet
address (`100.64.0.1`, user `ubuntu`) and `just infra::ssh_config` generates the
alias from it.

Runs:

- **k3s**, single node. `just infra::apply k3s-pve`
  (or `just infra::apply k3s-pve k3s-node` for that play alone).
- **ARC** (Actions Runner Controller) and the `ra8-ci` runner scale set,
  sized by the declaration, pods booting the pinned toolchain image.
  `just infra::apply k3s-pve ci-runner`.
- **OpenBao** -- the vault everything else reads credentials from.
- The owner's unrelated homelab (section 6).

### CT 107 `dev` -- the shared verification box

12 vCPU, 24 GB, 360 GB, Debian 12, unprivileged LXC with `nesting=1` (needed so
`just ci` can run podman inside it). `ssh dev`.

This is where every agent runs gates. Its whole toolchain is now codified
(`just infra::apply dev`), including two tools built from source because no
Debian suite carries them at the pinned version:

- **cppcheck** -- the parity gate compares its version *exactly*, because
  neighbouring releases emit version-specific false positives against this tree.
- **gcc** -- the second host-tool compiler arm, alongside clang.

Also: the pinned Arm GNU Toolchain and Unicorn, the pinned lint set,
`/etc/profile.d/ra8-ci.sh`, the shared ccache at `/var/cache/ccache-ra8`,
`~/ra8-ws` agent workspaces, and two systemd user units -- the shared CI status
poller and the workspace reaper.

**Never work directly in `~/ra8-firmware` on this box.** Use
`just workspace::new <name>`; improvised checkouts have clobbered other work.

### `truenas` -- NAS, and two CI runners

Plain (non-ARC) self-hosted runners in Docker containers, deployed by the
`ci_runner_docker` role. Measured roughly **2x faster per job** than a pve1 pod,
which is the oversubscription in section 1 stated from the other direction.

Everything it writes lives on a pool dataset, so `docker rm` is non-destructive
and a TrueNAS SCALE upgrade is survivable. The role refuses, by assertion, to
put CI I/O on the appliance's known-degraded 100T pool.

It is the one host class with a real one-command teardown:

```sh
just infra::remove truenas
```

### The gaming PC -- Ryzen 9 7900X, WSL2

Windows desktop running the `win-ci-*` runner instances under WSL2, carrying
the `ra8-ci` and `ra8-win` labels. Reachable only through the bench Pi,
which `infra/fleet.yml` declares as its `jump:` -- so any control node reaches
it, not just the Mac:

```sh
ssh win-ci                        # after `just infra::ssh_config`
ssh -J star sikar@10.0.40.103     # the same hop, spelled out
```

The most powerful CPU in the estate. Because it answers
`ra8-ci`, it absorbs load that would otherwise land on pve1.

### `star` -- the HIL bench Pi

Raspberry Pi 5, Ubuntu 24.04, `ssh star`. Everything physical hangs off it:

- the **EK-RA8D2** board over J-Link, plus `rfp-cli` for DLM recovery
- the **ESP32-C6** co-processor on PMOD1, with its own ESP-IDF toolchain
- the **Analog Discovery 2** logic analyser that probes the RA8 <-> C6 SPI lines
- the FortiGate console cable
- Tapo smart plugs for power-cycling the board and the Pi

`just infra::apply star` provisions all of it. One package cannot be
fetched unattended -- Digilent WaveForms sits behind a click-through licence
gate -- so the role **fails with download instructions** rather than skipping,
because a bench reporting success without `libdwf` would be a lie.

The GitHub workflow does not run on this Pi. Its dedicated dev-box listener
connects as the role-owned `ra8-hil` account with a single pinned SSH identity.
That account has no access to `star`'s mode-0600 TAPO/OpenBao configuration and
receives only the narrow privileged `ip`, `uhubctl`, and USB-authorisation
operations required by the existing HIL scripts.

The bench lock is also provisioned by that role. Its SSH liveness bound is
`ClientAliveInterval 15` with `ClientAliveCountMax 4` (about 60 seconds after a
client disappears without closing its socket), and the advisory overrun reaper
runs every five minutes. Verify the live deployment without touching firmware:

```sh
just hil::doctor
just hil::selftest
just hil::status
```

Provisioning must preserve the same lock contract as every HIL recipe: run
`just infra::check star` first, confirm `just hil::status` reports `FREE`, then
apply the role. The health-check path resets and halts the board; do not run a
full apply while another actor holds the rig.

### The bench LAN -- FortiGate 81E-POE + Meraki MR18

Two hard-switch segments share the FortiGate. The odd `lan` segment
(`10.0.40.0/24`) is denied WAN access by policy 2; the even `lan-even` segment
(`10.0.41.0/24`) NATs through `wan1`. The FortiGate supplies routing, DHCP,
switching, and PoE; the MR18 bridges `ra8-bench` only into the odd segment.

Not Ansible -- it is driven over the console cable from the bench Pi by
`infra/network/fg_bringup.py`, which reads every credential from OpenBao. See
`infra/network/README.md`.

---

## 3. Credentials -- all in OpenBao, none in this repo

The vault runs on the k3s node, reachable at the LAN NodePort `:32200`
(`BAO_ADDR`). Consumers authenticate with a read-only AppRole and a
`~/.config/hil/openbao.env` (mode 0600).

**Names only, values never:**

| Path | What reads it |
|---|---|
| `ra8/ci-runner-pat` | both CI runner roles, to register runners with GitHub |
| `secret/hil/tapo` | HIL smart-plug power control (`just hil::tapo`) |
| `secret/ra8d2/bench-network` | the FortiGate/AP bring-up -- admin creds, PSKs, and the chassis serial |

The FortiGate **chassis serial is treated as a credential**, not an asset tag:
FortiOS derives the console recovery password from it mechanically as
`bcpb<serial>`. It lives in the vault with everything else and appears nowhere
in this tree.

### The vault's own keys

`bao operator init` emits the Shamir unseal keys and the root token once. They
live in `~/.openbao/init.json` on the k3s node at mode 0600, outside every
checkout, and **their loss is unrecoverable** -- that is what a Shamir seal
means, and no backup of the vault's data substitutes for it.

Deployment is codified; initialising and unsealing are deliberately not, because
both produce secrets and a playbook that handles a root token can log one. The
`openbao` role deploys the vault, reports its seal state, and stops. The operator
steps are `scripts/secrets/README.md`.

A Shamir-sealed OpenBao comes up **sealed after every restart, by design**. If
something that reads a credential suddenly cannot, check
`just infra::status` first -- then
`/bin/bash -p scripts/secrets/openbao_unseal.sh`.

---

## 4. Rebuilding from nothing

Order matters, because each step is the next one's prerequisite.

```sh
just infra::doctor                      # can this machine drive any of it?
just infra::ssh_config                  # fleet host aliases from the declaration
just infra::setup                       # inventory + credentials (git-ignored)
just infra::check <host>                # ALWAYS dry-run first (`just infra::list`)
just infra::apply <host>
```

| # | Rebuild | Command | Notes |
|---|---|---|---|
| 1 | the Proxmox host | -- | **manual, not codified** (section 5) |
| 2 | VM 300 + CT 107 | -- | **manual, not codified** (section 5) |
| 3 | k3s + helm + vault | `just infra::apply k3s-pve k3s-node` | then init + unseal by hand |
| 4 | the ARC runner pool | `just infra::apply k3s-pve ci-runner` | needs 3 |
| 5 | the dev box | `just infra::apply dev` | slow: two source builds |
| 6 | extra runner hosts | `just infra::apply truenas` / `just infra::apply win-ci` | NAS, gaming PC |
| 7 | the HIL bench | `just infra::apply star` | needs the board attached |
| 8 | the bench LAN | `just infra::fortigate_bootstrap` | from `ssh star`; guarded confirmation |

**Where do you run these from?** Any machine with ansible and a key the hosts
accept. It used to be *nowhere*: every host was addressed by an `~/.ssh/config`
alias that existed on the Mac, which had no ansible, while the dev box had
ansible and could resolve none of them (#526). `infra/fleet.yml` now carries
each machine's real address, `fleet.py` builds every command from it, and
`just infra::ssh_config` generates the friendly aliases -- so becoming a control
node is `just setup-ansible`; Ansible core comes from `uv.lock` and
Galaxy collections come from `infra/ansible/requirements.yml`.
`just infra::doctor` says
which half you are missing.

The first run against a host takes far longer than later ones. `just infra::apply
dev` compiles gcc and cppcheck from source; re-runs skip both once the
pinned versions are present and cost a handful of version probes.

---

## 5. What is still NOT codified

Being honest about this is the point of the section.

- **The Proxmox guest topology.** VM 300 and CT 107 exist only as live config.
  If pve1 died tonight, both would be recreated from memory. The exact live
  configuration is recorded on issue #500 so a rebuild does not depend on
  anyone's recollection, and the intended shape of the role is described there.
- **Vault init and unseal.** Manual *by design*, not by omission -- see
  section 3.
- **`k3s-runner-maintenance.sh`** on the k3s node (weekly image / journal /
  build-dir housekeeping, on a systemd timer) is hand-installed and in no repo.
  Part of it is now dead: its build-dir step walks
  `/home/ubuntu/actions-runner*/_work`, and #502 removed those trees with the
  legacy runner pool. The loop is `nullglob`, so it reports "0 stale build/
  dir(s)" rather than failing, and the image-prune and journald steps are
  unaffected -- but a hand-installed script that silently lost a third of its
  job is the argument for codifying it, not against.
- **Physical cable placement at the FortiGate.** `fortigate-bench.conf`
  declares the odd/even port memberships, reservations, and policies, while
  `infra/network/README.md` records the Win11 client observed on port3. The repo
  cannot enforce where a cable is physically plugged in, so an operator must
  confirm placement before a replay or bench run.

### Known cruft, cleaned up (#502)

`just infra::status` used to show a pool of `k3s-runner*` runners registered
and online -- pre-ARC leftovers from before the repository was renamed,
carrying the bare `self-hosted,Linux,X64` labels.

They were **load-bearing, not dead**, which is why "confirm before removing"
was the right instinct: `docs-publish`, `fuzz-nightly` and `osv-scan` were
still scheduled against exactly those labels, so deregistering the pool would
have left those workflows with no runner that could serve them. That was the
unfinished half of the ARC migration, which moved the core workflows to
`ra8-ci` but left the stragglers behind "until their tools are confirmed in
the image" and never revisited the condition.

The condition was checked against a live pod, the stragglers were moved to
`ra8-ci`, and only then was the pool retired -- registrations deleted, units
stopped and removed with their drop-in directories, and the runner installs
reclaimed from disk. The per-workflow dependency evidence is in
`infra/README.md` under "The legacy `k3s-runner-*` pool is retired".

---

## 6. Present, and deliberately out of scope

The k3s node also hosts the owner's personal homelab: the media stack (jellyfin,
*arr, immich, audiobookshelf), authentik, a hugo blog, ntfy, paperless,
vaultwarden, minecraft, headscale, pihole, grafana/prometheus/loki.

**None of it is this project's concern.** It is listed here only so nobody
later assumes it was forgotten, or "helpfully" folds it into these roles. It
shares hardware with CI and nothing else -- though it is worth remembering that
it also shares that oversubscribed CPU, and its steady-state draw is part of
the arithmetic in `ci_runner`'s defaults.

---

## See also

- `infra/README.md` -- per-role index, and the runner-pool topology in detail
- `infra/network/README.md` -- the split odd-islanded/even-uplinked bench LAN
- `scripts/secrets/README.md` -- vault init, unseal, and secret configuration
- `docs/TOOLCHAIN.md` -- what the pinned versions are and why
- `just infra` -- the command surface
