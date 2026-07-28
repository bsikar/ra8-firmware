# The estate

Every machine this project runs on: what it is, what it does, how it relates to
the others, and how to rebuild it from nothing.

This is the document to read when you have forgotten how any of it works. It is
deliberately narrative rather than a role-by-role reference -- the roles
document themselves, and `infra/README.md` is the per-role index.

**One command orients you:**

```sh
make infra-status     # what is deployed across the estate, right now (read-only)
make infra-list       # what machines are declared, and how they are sized
make infra-doctor     # can THIS machine drive any of it?
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
   |   |  - k3s (single node)      |   |  - every agent's `make ci`    |   |   |
   |   |  - ARC runner pool        |   |  - the pinned host toolchain  |   |   |
   |   |  - OpenBao vault          |   |  - ~/ra8-ws agent workspaces  |   |   |
   |   |  - the homelab (see 6)    |   |  - shared ccache              |   |   |
   |   +---------------------------+   +-------------------------------+   |   |
   |            16 vCPU        +        12 vCPU     =  28 vCPU  on 16      |   |
   +-----------------------------------------------------------------------+   |
                                                                               |
   Independent machines (NOT on pve1):                                         |
                                                                               |
   truenas ....... NAS + one CI runner in Docker                               |
   gaming PC ..... Ryzen 9 7900X, Windows + WSL2, three CI runners             |
   star .......... Raspberry Pi -- the HIL bench (board, J-Link, C6, AD2)      |
   FortiGate+AP .. the isolated 10.0.40.0/24 bench LAN (no uplink, by design)  |
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

- **Adding ARC pods does not add throughput.** `ci_runner_max` is deliberately
  6, not 10. The reasoning -- with the measured numbers -- is written at length
  in `infra/ansible/roles/ci_runner/defaults/main.yml`. Read it before raising
  that value; the answer is almost certainly "no".
- **Real capacity comes from machines that are not pve1.** That is exactly what
  `truenas` and the gaming PC are for. Both answer the same `ra8-ci` label, so
  GitHub spreads `runs-on: ra8-ci` across four independent machines instead of
  piling it onto the one that is already oversubscribed at the hypervisor level.

If pve1's headroom ever genuinely improves, the number to re-check first is
total vCPU commitment on the physical host -- not any single guest's setting.

---

## 2. The machines, one at a time

### `pve1` -- the hypervisor

Proxmox 8.4.19. `ssh pve`, user `pve-admin`, passwordless sudo. Bridges:
`vmbr0` 192.168.1.50/24 (LAN) and `vmbr1` 10.10.10.2/29 (a 2.5 Gb link to
TrueNAS, MTU 9000). Storage is `local` (dir) plus `local-lvm` (lvmthin, ~1.8 TB).

**Not codified.** The guest definitions exist only as live config. This is the
one remaining hole in the rebuild story -- see section 5.

### VM 300 `k3s` -- CI cluster and vault

16 vCPU, 64 GB, 500 GB, Ubuntu 24.04, two PCIe devices passed through. Reachable
as `ssh k3s-pve`, **from the Mac only** -- the dev box has no key for it and does
not even resolve the name.

Runs:

- **k3s** v1.34.5+k3s1, single node. `make infra-apply HOST=k3s`.
- **ARC** (Actions Runner Controller) and the `ra8-ci` runner scale set, min 0 /
  max 6, pods booting the pinned toolchain image.
  `make infra-apply HOST=ci-runner`.
- **OpenBao** -- the vault everything else reads credentials from.
- The owner's unrelated homelab (section 6).

### CT 107 `dev` -- the shared verification box

12 vCPU, 24 GB, 360 GB, Debian 12, unprivileged LXC with `nesting=1` (needed so
`make ci` can run podman inside it). `ssh dev`.

This is where every agent runs gates. Its whole toolchain is now codified
(`make infra-apply HOST=dev`), including two tools built from source because no
Debian suite carries them at the pinned version:

- **cppcheck 2.13.0** -- the parity gate compares it *exactly*, because
  neighbouring releases emit version-specific false positives against this tree.
- **gcc 14.2.0** -- the second host-tool compiler arm, alongside clang-18.

Also: the Arm GNU Toolchain 13.3.rel1, Unicorn 2.1.4, the pinned lint set,
`/etc/profile.d/ra8-ci.sh`, the shared 30 GB ccache at `/var/cache/ccache-ra8`,
`~/ra8-ws` agent workspaces, and two systemd user units -- the shared CI status
poller and the workspace reaper.

**Never work directly in `~/ra8-firmware` on this box.** Use `make ws-new
NAME=...`; improvised checkouts have clobbered other agents' work.

### `truenas` -- NAS, and a CI runner

A plain (non-ARC) self-hosted runner in a Docker container, deployed by the
`ci_runner_docker` role. Measured roughly **2x faster per job** than a pve1 pod,
which is the oversubscription in section 1 stated from the other direction.

Everything it writes lives on a pool dataset, so `docker rm` is non-destructive
and a TrueNAS SCALE upgrade is survivable. The role refuses, by assertion, to
put CI I/O on the appliance's known-degraded 100T pool.

It is the one host class with a real one-command teardown:

```sh
make infra-remove HOST=ci-runner-docker
```

### The gaming PC -- Ryzen 9 7900X, WSL2

Windows desktop running three runner instances (`win-ci-1/2/3`) under WSL2,
carrying the `ra8-ci` and `ra8-win` labels. Reachable **from the Mac only**, and
only through the bench Pi:

```sh
ssh -J star sikar@10.0.40.100
```

The most powerful CPU in the estate and the newest addition. Because it answers
`ra8-ci`, it absorbs load that would otherwise land on pve1.

### `star` -- the HIL bench Pi

Raspberry Pi 5, Ubuntu 24.04, `ssh star`. Everything physical hangs off it:

- the **EK-RA8D2** board over J-Link (SEGGER V9.42) and `rfp-cli` for DLM
  recovery
- the **ESP32-C6** co-processor on PMOD1, with its own ESP-IDF toolchain
- the **Analog Discovery 2** logic analyser that probes the RA8 <-> C6 SPI lines
- the FortiGate console cable
- Tapo smart plugs for power-cycling the board and the Pi

`make infra-apply HOST=bench` provisions all of it. One package cannot be
fetched unattended -- Digilent WaveForms sits behind a click-through licence
gate -- so the role **fails with download instructions** rather than skipping,
because a bench reporting success without `libdwf` would be a lie.

### The bench LAN -- FortiGate 81E-POE + Meraki MR18

An islanded 10.0.40.0/24 network with **no uplink at all**: no WAN, no default
route. The FortiGate is router/DHCP/switch and PoE source; the MR18 (running
OpenWrt) is a dumb AP beaconing `ra8-bench` on 2.4 GHz for the ESP32-C6 to join.

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
| `secret/hil/tapo` | HIL smart-plug power control (`make hil-tapo`) |
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
`make infra-status` first -- then `scripts/secrets/openbao_unseal.sh`.

---

## 4. Rebuilding from nothing

Order matters, because each step is the next one's prerequisite.

```sh
make infra-doctor                      # can this machine drive any of it?
make infra-setup                       # inventory + credentials (git-ignored)
make infra-check HOST=<class>          # ALWAYS dry-run first
make infra-apply HOST=<class>
```

| # | Rebuild | Command | Notes |
|---|---|---|---|
| 1 | the Proxmox host | -- | **manual, not codified** (section 5) |
| 2 | VM 300 + CT 107 | -- | **manual, not codified** (section 5) |
| 3 | k3s + helm + vault | `make infra-apply HOST=k3s` | then init + unseal by hand |
| 4 | the ARC runner pool | `make infra-apply HOST=ci-runner` | needs 3 |
| 5 | the dev box | `make infra-apply HOST=dev` | slow: two source builds |
| 6 | extra runner hosts | `make infra-apply HOST=ci-runner-docker` | NAS, gaming PC |
| 7 | the HIL bench | `make infra-apply HOST=bench` | needs the board attached |
| 8 | the bench LAN | `python3 infra/network/fg_bringup.py bootstrap` | from `ssh star` |

**Where do you run these from?** Whichever machine has *both* ansible and ssh
reach. Today that is nowhere by default, which `make infra-doctor` will tell you
bluntly: the Mac reaches every host but has no ansible, and the dev box has
ansible but cannot resolve the cluster hosts. Install ansible where the ssh
access already is.

The first run of a class takes far longer than later ones. `make infra-apply
HOST=dev` compiles gcc and cppcheck from source; re-runs skip both once the
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
  One third of it is now dead: its build-dir step walks
  `/home/ubuntu/actions-runner*/_work`, and #502 removed those trees with the
  legacy runner pool. The loop is `nullglob`, so it reports "0 stale build/
  dir(s)" rather than failing, and the image-prune and journald steps are
  unaffected -- but a hand-installed script that silently lost a third of its
  job is the argument for codifying it, not against.
- **FortiGate port assignments** are not recorded in `infra/network/`. The unit
  is on an islanded LAN reachable only through the bench Pi console, so this was
  left rather than written down unverified.

### Known cruft, cleaned up (#502)

`make infra-status` used to show seven `k3s-runner*` runners registered and
online -- pre-ARC leftovers from before the repository was renamed
(`ra8d2-firmware`), carrying the bare `self-hosted,Linux,X64` labels.

They were **load-bearing, not dead**, which is why "confirm before removing"
was the right instinct: `docs-publish`, `fuzz-nightly` and `osv-scan` were
still scheduled against exactly those labels, so deregistering the pool would
have left three workflows with no runner that could serve them. That was the
unfinished half of the Jul-24 ARC migration, which moved the core workflows to
`ra8-ci` but left those three behind "until their tools are confirmed in the
image" and never revisited the condition.

The condition was checked against a live pod, all three moved to `ra8-ci`, and
only then was the pool retired: seven registrations deleted, all 20
`actions.runner.*` units stopped, disabled and removed with their drop-in
directories, and 81 GB of runner installs reclaimed from `/home/ubuntu`. The
per-workflow dependency evidence is in `infra/README.md` under "The legacy
`k3s-runner-*` pool is retired".

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
- `infra/network/README.md` -- the isolated bench LAN, in full
- `scripts/secrets/README.md` -- vault init, unseal, and secret configuration
- `docs/TOOLCHAIN.md` -- what the pinned versions are and why
- `make infra-help` -- the command surface
