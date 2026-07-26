# infra/images -- CI runner container image

`runner/Dockerfile` builds the image ARC runner pods boot from: the pinned
devcontainer toolchain (`FROM` the `.devcontainer` image) plus the GitHub
Actions runner and its ARC k8s hooks. Every CI job therefore runs the exact
`make ci` toolchain.

**The Ansible `ci_runner` role is authoritative** for how that image reaches a
node (`infra/ansible/roles/ci_runner/tasks/main.yml`). Everything below mirrors
it step for step so it can be run by hand; if the two ever disagree, the role
is right and this file is stale. Deploy with the role rather than by hand
whenever you have the choice -- it carries the asserts.

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
  directory; the hand steps below apply it explicitly because a manual
  `ctr images import` does not go through that code path.

Two rules the steps encode, both learned from #484:

1. **The archive's embedded tag must equal the tag the scale set boots.** A
   hand-built archive tagged `:latest` imported cleanly while the deployment
   wanted `:v2`, so the import "worked" and the pods still could not find their
   image.
2. **Write the archive somewhere else and rename it in.** The watcher fires on
   WRITE, so exporting a multi-GB archive straight into the watched directory
   makes k3s repeatedly import a half-written file (measured: nine consecutive
   `unexpected EOF` imports over one four-minute export). A rename is atomic.

## Build and install by hand

```
IMAGE=localhost/ra8-ci-runner:v2
STAGE=/var/lib/rancher/k3s/agent/ra8-image-staging/ra8-ci-runner.tar
ARCHIVE=/var/lib/rancher/k3s/agent/images/ra8-ci-runner.tar

sudo install -d -o root -g root -m 0755 "$(dirname "$STAGE")" "$(dirname "$ARCHIVE")"

buildah bud -t localhost/ra8-devcontainer:latest \
  -f ../../../.devcontainer/Dockerfile ../../../.devcontainer
buildah bud --build-arg BASE_IMAGE=localhost/ra8-devcontainer:latest \
  -t "$IMAGE" -f runner/Dockerfile runner
sudo buildah push "$IMAGE" "docker-archive:$STAGE:$IMAGE"

# Assert the archive claims the tag the deployment boots, BEFORE publishing it.
sudo tar -xOf "$STAGE" manifest.json \
  | python3 -c 'import json,sys; print("\n".join(t for e in json.load(sys.stdin) for t in (e.get("RepoTags") or [])))' \
  | grep -qx "$IMAGE" || { echo "archive tag != $IMAGE -- do not import"; exit 1; }

sudo mv -f "$STAGE" "$ARCHIVE"                       # atomic; one watcher event
sudo k3s ctr -n k8s.io images import "$ARCHIVE"      # converge now, no restart
sudo k3s ctr -n k8s.io images label "$IMAGE" io.cri-containerd.pinned=pinned

# Assert the exact ref is present and pinned.
sudo k3s ctr -n k8s.io images ls "name==$IMAGE"
```

The last command must print one row for `$IMAGE` whose LABELS column contains
`io.cri-containerd.pinned=pinned`. Anything else means the pods will not start.

## Emergency recovery -- pods stuck in ErrImageNeverPull

Symptom: `kubectl -n arc-runners get pods` shows `ErrImageNeverPull` (or
`ImagePullBackOff` on an image named `localhost/...`), every workflow run sits
`queued` indefinitely, and `make ci-status` reports the queue stalled. The node
lost the image; the pods cannot pull it back because that is what
`imagePullPolicy: Never` means.

```
# 1. Is the image there at all, and is it pinned?
sudo k3s ctr -n k8s.io images ls "name==localhost/ra8-ci-runner:v2"

# 2. Is the durable archive there? (~3.6 GiB)
ls -l /var/lib/rancher/k3s/agent/images/ra8-ci-runner.tar

# 3. Re-import it. k3s does this itself on restart, but this needs no restart
#    and does not disturb jobs already running.
sudo k3s ctr -n k8s.io images import /var/lib/rancher/k3s/agent/images/ra8-ci-runner.tar
sudo k3s ctr -n k8s.io images label localhost/ra8-ci-runner:v2 \
  io.cri-containerd.pinned=pinned

# 4. Confirm, then let ARC create fresh pods (it retries on its own).
sudo k3s ctr -n k8s.io images ls "name==localhost/ra8-ci-runner:v2"
sudo k3s kubectl -n arc-runners get pods
```

Gotchas, all of them observed during the #484 outage:

- **Check the tag on whatever you import.** If step 3 was run against an
  archive from somewhere else, verify what it actually contains
  (`tar -xOf <tar> manifest.json`) before trusting it. An archive tagged
  `:latest` imports fine and leaves the pods exactly as broken as before; the
  stopgap is `sudo k3s ctr -n k8s.io images tag localhost/ra8-ci-runner:latest
  localhost/ra8-ci-runner:v2`, and the fix is to rebuild the archive correctly.
- **Check the pin label, not just the ref.** An unpinned image is one GC sweep
  from the same outage.
- **Do not run the export into the watched directory** while recovering -- see
  the staging rule above.
- If the archive itself is gone, rebuild it from the image still in containerd:
  `sudo k3s ctr -n k8s.io images export <stage-path> localhost/ra8-ci-runner:v2`
  (this preserves the ref), then `mv` it into place. If containerd has nothing
  either, run the full build above -- or just re-run the `ci_runner` role.
