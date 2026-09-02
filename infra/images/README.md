# infra/images -- CI runner container image

`runner/Dockerfile` builds the image ARC runner pods boot from: the pinned
devcontainer toolchain (`FROM` the `.devcontainer` image) plus the GitHub
Actions runner and its ARC k8s hooks. Every CI job therefore runs the exact
`just ci` toolchain.

**The declared Ansible path is authoritative** for how that image reaches every
runner. `infra/fleet.yml` names the producer, image reference and durable
archive; `scripts/dev/fleet.py` derives the play order and variables; and the
`ci_runner` role builds, validates, publishes and imports the artifact. There
is intentionally no second hand-build recipe here for those steps to drift
against.

The role **stages the build context onto the node from the control node's
checkout** (`/var/lib/ra8-ci/build-context`) and builds from there, so the image
comes from the same tree whose gates then assert what it contains. It used to
build from a path on the node that nothing created or synced, which is how the
deployed image came to be two pinned-tool changes behind this Dockerfile
(#513). Converge the declared producer through the fleet front door:

```sh
just infra::check k3s-pve
just infra::apply k3s-pve
```

The apply also converges the prerequisite k3s play because both provisions are
declared on that host. Do not invoke the role playbook or reproduce its image
commands by hand.

## Where the image lives on the node

There is no registry on a single-node k3s, so the pods pull nothing
(`imagePullPolicy: Never`) and the image must already be in the node's
containerd. Two things keep it there:

- **`/var/lib/rancher/k3s/agent/images/ra8-ci-runner.tar`** -- k3s imports
  every archive in this directory at agent start, and an fsnotify watcher
  imports one again whenever it appears or changes. An image evicted from
  containerd therefore comes back on its own. This is the durable copy; it is
  the reason `/tmp` is never used (nothing re-imports from there, and
  systemd-tmpfiles clears it).
- **`io.cri-containerd.pinned=pinned`** on the image -- containerd's CRI
  garbage collector skips pinned images, so eviction should not happen in the
  first place. k3s applies this label itself to everything it imports from that
  directory, and the role asserts the label after its explicit import.

Two rules the role encodes, both learned from #484:

1. **The archive's embedded tag must equal the tag the scale set boots.** A
   hand-built archive tagged `:latest` imported cleanly while the deployment
   wanted `:v2`, so the import "worked" and the pods still could not find their
   image.
2. **Write the archive somewhere else and rename it in.** The watcher fires on
   WRITE, so exporting a multi-GB archive straight into the watched directory
   makes k3s repeatedly import the half-written file and fail on `unexpected
   EOF` for as long as the export runs. A rename is atomic.

## Emergency recovery -- pods stuck in ErrImageNeverPull

Symptom: `kubectl -n arc-runners get pods` shows `ErrImageNeverPull` (or
`ImagePullBackOff` on an image named `localhost/...`), every workflow run sits
`queued` indefinitely, and `just quality::local::gate ci-status-contract` reports the queue stalled. The node
lost the image; the pods cannot pull it back because that is what
`imagePullPolicy: Never` means. Diagnose with read-only probes, then converge
the producer through Ansible; the role rebuilds or republishes the archive,
imports it, pins it and verifies the exact reference in one operation.

```
# 1. Is the image there at all, and is it pinned?
sudo k3s ctr -n k8s.io images ls "name==localhost/ra8-ci-runner:v2"

# 2. Is the durable archive there? (it is multi-GB)
ls -l /var/lib/rancher/k3s/agent/images/ra8-ci-runner.tar

# 3. From a control node, restore the declared state. Do not reconstruct the
#    role's build/import sequence at the console.
just infra::apply k3s-pve

# 4. Confirm, then let ARC create fresh pods (it retries on its own).
sudo k3s ctr -n k8s.io images ls "name==localhost/ra8-ci-runner:v2"
sudo k3s kubectl -n arc-runners get pods
```

Gotchas, all of them observed during the #484 outage:

- **Do not import an archive from somewhere else.** The fleet declaration names
  the one canonical archive and the role rejects it before publication unless
  its embedded tag matches the image the scale set boots.
- **Check the pin label, not just the ref.** An unpinned image is one GC sweep
  from the same outage.
- **Do not run an export into the watched directory.** The Ansible role writes
  elsewhere and atomically renames the verified archive into place.
- If the archive and containerd image are both gone, the same fleet apply is
  still the recovery path; it rebuilds them from the checked-out source.
