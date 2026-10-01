# infra/ -- reproducible rig as code

Provisions and configures every machine the project runs on -- dev boxes, CI
runners, and the hardware-in-the-loop (HIL) bench -- from code, so an
environment is recreated instead of hand-assembled.

## Quick start (clone -> deploy)

```
just infra::setup          # or: /bin/bash -p infra/bootstrap.sh
```

The bootstrap checks prerequisites, writes your **git-ignored** inventory, names
the fleet's machines in your `~/.ssh/config` (generated from `fleet.yml`, so
this machine can drive the estate without anyone hand-copying aliases -- see
`docs/CI_FLEET.md`), stores your GitHub token, and offers to run the deploy.
Nothing secret is ever committed -- your token lands in
`infra/ansible/private/` (git-ignored) or, if you run one, in OpenBao. That's
it: your machine joins as a CI runner pool.

## What it does

- **Dev box** -- one playbook installs the exact pinned toolchain, so `just ci`
  locally matches CI.
- **CI runner** -- point a spare machine (or a friend's server) at the repo and
  it joins the runner pool: more hardware, more parallel CI. Two shapes are
  supported and can run side by side -- an autoscaling ARC pool on a k8s
  cluster, and long-lived containers on any plain Docker host.
- **HIL bench** -- configures the bench Pi's J-Link, `rfp-cli`, serial console,
  smart-plug control, the ESP32-C6 build toolchain, and the Digilent Analog
  Discovery 2 logic analyzer used to probe the RA8 <-> C6 SPI lines.
- **Throwaway environments** -- create and configure new LXCs, including
  per-agent isolated dev containers.

## Layout

```
fleet.yml    THE declaration: one block per machine -- its ADDRESS (an IP or a
             resolvable name, never an ssh alias), what kind of host it is, and
             which plays it runs. Everything below is derived from it, as is
             the ~/.ssh fragment `just infra::ssh_config` installs.
ansible/     configures machines (dev_box, k3s_node, openbao, hil_bench,
             c6_toolchain, ad2_tools)
network/     the isolated ESP32-C6 bench LAN (FortiGate + OpenWrt AP)
```

`ansible/inventory/hosts.ini` is **generated** from `fleet.yml` on every
`just infra::*` run and is git-ignored; the committed half is
`ansible/inventory/host_vars/`, which holds structural facts about a machine
and never a knob the declaration owns. `scripts/checks/check_fleet_declaration.py` fails a `host_vars` file that
re-declares anything `fleet.yml` owns.

**Which machines there are and what each runs are questions only `fleet.yml`
answers.** Nothing in this file restates them.

**Adding a machine, retuning one, removing one:
[`docs/CI_FLEET.md`](../docs/CI_FLEET.md).**

## What is codified, and what is not

The rule this directory exists to serve is that **anything hand-installed is
lost at the next re-provision**. That makes the honest status per role part of
the documentation, not a footnote:

| Role / service | Status |
|---|---|
| `dev_box` (dev / verification box) | codified |
| `k3s_node` (cluster + `helm`) | codified |
| `openbao` (vault deployment) | codified |
| `hil_bench`, `c6_toolchain`, `ad2_tools` (bench Pi) | codified |
| `network/` (bench LAN: FortiGate + AP) | codified |
| vault init / unseal / secrets (`scripts/secrets/`) | manual **by design** |
| Proxmox guest topology | **hand-built** |

Two rows deserve their exact wording. Vault initialisation and unsealing are
manual *by design*, not by omission: both produce secrets, and a playbook that
handles a root token can log one. The Proxmox row is a genuine open gap -- the
VM and LXC definitions the whole rig sits on exist only as live guest config.

### Order of operations on a bare cluster

`k3s_node` builds the cluster and installs `helm`; `openbao` deploys the vault
onto it. A host that declares both plays runs them in that order:

```
just infra::apply k3s-pve
```

For focused role debugging, the dispatcher can select one declared play, for
example `just infra::check k3s-pve k3s-node`; routine convergence should keep
the declaration's full order with the unqualified Just command.

k3s is pinned by release version *and* by the sha256 of the release binary, and
the upstream installer is fetched to disk and checksummed rather than piped
into a shell -- the same discipline the devcontainer applies to every download.
When upstream re-cuts `get.k3s.io` the checksum stops matching and the role
fails with the new digest, which is what a pin is for.

The vault is deployed but never initialised or unsealed. The role reports which
of the two the operator still owes it; the steps are in
`scripts/secrets/README.md`.

## The dev box

The machine agents run `just ci` / `just quality::native` on. It also carries
one repo-scoped native HIL listener; that listener builds firmware here and
uses the existing SSH drivers to operate the bench declared in `fleet.yml`.
The listener runs as a dedicated system account with `ProtectHome=true`, a
private runner root, and a single SSH key accepted only by an equally isolated
bench account; it cannot read the normal users' GitHub, SSH, TAPO or OpenBao
credentials. It is not general runner capacity, so the machine remains in its
own inventory group and playbook:

```
just infra::apply dev
```

The listener uses its own `/var/cache/ccache-ra8-hil` compiler cache, owned by
`ra8-hil:ra8-hil` with mode 0700. It never receives ACL or write access to the
interactive and general-CI cache at `/var/cache/ccache-ra8`. This separation is
a trust boundary: firmware checkout content running as the HIL service cannot
replace compiler objects later consumed by a human or a general CI job, and
those users cannot replace objects consumed by HIL.

Every HIL-owned shell entry and the generated CI monitor service enter through
the fixed `/bin/bash -p` interpreter. The absolute privileged-mode boundary is
deliberate on the supported Mac/Linux/WSL/bench hosts: it prevents inherited
`BASH_ENV` and exported shell functions from executing before the reviewed
entrypoint, and nested HIL callers preserve the same boundary.

To create or repair only the private cache on an already-provisioned listener, use the
fixed standalone front door from the repository root:

```sh
just --justfile infra/hil-cache.just check
just --justfile infra/hil-cache.just apply
```

This explicit Justfile has no imports, variable exports, or backticks; its only
setting disables dotenv loading. The repository's ordinary root and module
files are therefore not parsed before the repair driver starts.

The integrity checker treats repair entrypoint ownership as a first-party
Git-authored token census, not as command-file classification. It validates the
inherited live index's modes and blob bytes, then independently validates the
present tracked and non-ignored untracked worktree bytes. Thus staged content
cannot be hidden by replacing or deleting its worktree file, while unstaged
content cannot be hidden behind a safe staged blob. A staged deletion disappears
only from the index view; retained non-ignored worktree bytes remain in scope.
Ignored build output does not enter either view. Regardless of filename, suffix,
or executable mode, every file outside explicit vendor, generated, build, cache,
and dependency trees is checked as raw bytes for alternate references in UTF-8,
UTF-16LE, and UTF-16BE. First-party symbolic links and any Git warning or
file-inspection error fail closed. Its selftest pins the two-view inventory,
mismatched index/worktree bytes and modes, raw encodings, staged deletions,
failure behavior, and every excluded tree boundary.

The repair playbook is deliberately separate from the broad `dev_box` role and
accepts no host, play, tag, or extra-variable argument. It targets only the
fleet-declared `dev` machine, uses the literal `/var/cache/ccache-ra8-hil` path
and existing literal `ra8-hil` account, and rejects UID 0, a symlink, or any
non-directory cache root before setting owner `ra8-hil:ra8-hil` and mode 0700.
It then creates and removes one cache-local throwaway file as `ra8-hil`. It
refuses to bootstrap a
missing identity and cannot stage the toolchain, install packages, create
accounts, contact the bench, register/restart the Actions listener, or touch
hardware because none of those roles or tasks is present in the standalone
playbook.

The driver copies only that playbook and the generated inventory into a
private temporary directory and supplies its own minimal Ansible config. This
prevents `host_vars`, `group_vars`, role tags, special `always` tags, or dynamic
include inheritance from widening the repair. The checker integrity-pins the
declared `dev` connection without printing its private endpoint, and the driver
refuses every inherited `ANSIBLE_*` control variable before it starts Ansible;
its private config is the only source allowed to set Ansible controls.
Host-key checking remains on. The dry run does not create the throwaway file.
If the cache root is absent, the dry run reports its pending creation and skips
the child config whose parent intentionally does not exist in check mode; apply
creates the root and its private config before the acceptance probe. The
standalone repair does not update or restart the service. Use the complete
listener converge below to migrate its environment to the private cache.

After registration, inspect the focused listener slice with a dry run. A real
converge must enter the complete authenticated dev-box transaction:

```sh
just infra::check dev dev-box hil-runner
just --yes infra::apply dev dev-box
```

The dry run may select `hil-runner` because it changes nothing. The real apply
deliberately rejects public tag selectors: the dispatcher previews the complete
play, authenticates the whole-bench hold, and stops only an idle listener
before any dev-box mutation. First registration uses the separately typed
`infra::register_hil` path.

The `hosts.dev.hil_runner` block is the single source for its registration
name, repository, custom labels, owned workflow, and bench relationship. The
fleet dispatcher derives the corresponding `dev_box_hil_runner_*` role inputs,
and the declaration checker rejects both a workflow-side and a
declaration-side label change that is not made at the other end.

An already registered listener converges with the ordinary command above and
must not be given another token. Only its first registration needs a short-lived
GitHub repository runner-registration token. Put it in a mode-0600 temporary
Ansible vars file outside the checkout, never in a command-line value:

```sh
hil_registration_vars="$(mktemp /tmp/ra8-hil-registration.XXXXXX)"
trap 'rm -f -- "${hil_registration_vars}"' EXIT
${EDITOR:-vi} "${hil_registration_vars}"
# Add one YAML mapping in the editor:
# dev_box_hil_runner_registration_token: "<ephemeral registration token>"
just infra::register_hil "${hil_registration_vars}"
```

This narrowly typed recipe accepts only the mode-0600 registration vars file;
it does not expose arbitrary Ansible arguments. Ordinary targeted checks and
convergence use the Just commands above.

The temporary file is created 0600 by `mktemp`; delete it as soon as the
one-time registration succeeds. The role fails loudly without this input only
when no `.runner` registration exists. It never writes the token into the
runner environment, systemd unit, or repository.

One of its tools is built from source, and that is a property of the
distribution rather than a preference. The **second host-tool compiler arm** is a gcc major that Debian
12 does not package at all. It is pinned by URL + sha256 and asserted
afterwards, the same discipline as every vendor download here. A first run
therefore takes tens of minutes; re-runs skip the build once the pinned
version is present.

The role reads the *other* pins rather than restating them -- lint and format
versions from `.devcontainer/Dockerfile`, the Unicorn pin from
`scripts/ci/unicorn_pin.sh`, and the two systemd units from the scripts that
generate them -- and finishes on `check_tool_versions.py --all`, the exact
assertion the `toolchain-parity` gate makes. A box that cannot reach parity
fails the play.

## There is no runner pool

CI is `tools/ra8ci`. The ARC scale set on the k3s node, the NAS and WSL Docker
runners, and the runner image they booted are all removed. The only GitHub
runner left is the dedicated HIL listener on the dev box, labelled
`[self-hosted, hil, ra8d2]`, which drives the remote physical bench.

## The legacy `k3s-runner-*` pool is retired

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

`.devcontainer/Dockerfile` pins every host tool. `just ci` and the `dev_box`
role both consume those pins; the
`toolchain-parity` gate fails if any execution environment drifts from them.

The Debian dev box has two non-overlapping provisioning layers. Its Ansible
role owns apt packages and the GCC source build. The role then calls
`scripts/dev/provision_dev_box_toolchain.sh`, the one definition for pinned
release binaries and the isolated Python gate-tool venv. CI runners never call
that host helper: their complete container image is built and deployed by the
Ansible runner roles. The helper installs the pinned **doxygen** release the
same way the Dockerfile does (download, sha256, `/usr/local/bin`), because
`toolchain-parity` now compares that version too: it did not, which is why a
major doxygen drift stayed invisible to the one gate whose job is catching
exactly that. The pin is asserted only on the architectures the
Dockerfile pins it for -- doxygen publishes no official linux-arm64 binary, so
an arm64 `just ci` container keeps apt's.

The dev-box role stages its recipes and `.devcontainer` input from the control
node's candidate worktree, not from the intentionally non-updating base checkout
on the managed host. `scripts/dev/stage_worktree_context.py` builds a
deterministic archive from present cached files plus non-ignored untracked
files. It reads current worktree bytes, preserves portable executable and
symlink modes, and excludes ignored caches and worktree-deleted paths. The role
verifies the extracted tree exactly before reuse, so extra local residue also
forces replacement. This is why a dirty pre-commit candidate is supported
without allowing `__pycache__`, `.pyc`, or other ignored workstation output to
affect the provisioned image context.

The installed verifier remains a package rather than a copied standalone
script. Its entry point and `git_environment.py` runtime dependency are built
together in a new root-private release directory under `/usr/local/libexec`.
The role proves the exact two-file census, ownership, modes, controller-source
SHA-256 digests, and import behavior before making the directory readable and
atomically switching `/usr/local/bin/ra8-stage-worktree-context` to it. A stale,
misowned, writable, interrupted, or extra-file release is never repaired or
executed in place. Failed assembly leaves the prior command untouched, and a
successful publication removes inactive managed releases and staging residue.

The **image `just ci` boots** is pinned by the same file and now derived from
it: `scripts/ci/devcontainer_image.sh` stamps a sha256 of the whole
`.devcontainer/` build context onto the image as a label and compares it back,
so a cached image built from a different Dockerfile is rebuilt rather than
reused. `just ci` calls it on every run -- which is what covers the Mac, where
no Ansible play ever lands -- and the `dev_box` role calls the same script so
`just infra::apply dev` leaves the box warm. Before that, the box booted a stale image under a newer tree and reported
gates red that passed natively on the same commit.

Image-build serialization has a separate managed lock authority at
`/var/cache/ra8-devcontainer-image-lock`. The role owns the directory as
`root:<dev-box-user-primary-group>` mode 0750 and pre-creates its single-link
regular lock as mode 0660. Both callers therefore lock the same inode,
but the interactive account cannot replace its pathname. A root:root mode-0444
single-link marker records that account's numeric primary gid; runtime requires
the directory and lock gids to equal that non-root value. Do not move this file
into a sticky mode-1777 cache: with Linux `fs.protected_regular=2`, opening an
existing file owned by the other caller with O_CREAT is denied by design. Away
from an Ansible-managed dev box, the image helper uses a mode-0700 per-user
directory below `XDG_CACHE_HOME` or `$HOME/.cache`; macOS remains supported
without requiring `flock`. On a managed box the helper discovers the canonical
authority even in non-login shells, then validates the marker, numeric identities
and exact modes and opens the pre-created file without O_CREAT. The profile
export is a second authority and is deliberately absent: explicit command
environments or canonical discovery are the only managed paths. Normal and
forced rebuilds both take the same lock and validate that the opened inode still
matches the checked path.

## Secrets

Fleet topology, including routable host addresses, is committed in
`infra/fleet.yml`; generated Ansible inventory is git-ignored. Tokens and other
credentials are never part of either file. Long-lived secrets live in OpenBao
or git-ignored `ansible/private/`, while one-time registration inputs use the
mode-0600 external vars-file procedure above. Run `just infra::setup` to prepare
a control node and `just infra::apply <host>` to converge a declaration.

The HIL bench play also loads `ansible/private/secrets.yml` when present. Set
`ad2_tools_device_serial` there so the role can distinguish the intended
instrument from another USB device with the same VID:PID without committing a
unique asset identifier.
