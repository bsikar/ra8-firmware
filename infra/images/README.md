# infra/images -- CI runner container image

`runner/Dockerfile` builds the image ARC runner pods boot from: the pinned
devcontainer toolchain (`FROM` the `.devcontainer` image) plus the GitHub
Actions runner and its ARC k8s hooks. Every CI job therefore runs the exact
`make ci` toolchain.

The Ansible `ci-runner` role builds and imports it. To build by hand on a k3s
node (no registry -- imported straight into containerd):

```
buildah bud -t localhost/ra8-devcontainer:latest \
  -f ../../../.devcontainer/Dockerfile ../../../.devcontainer
buildah bud --build-arg BASE_IMAGE=localhost/ra8-devcontainer:latest \
  -t localhost/ra8-ci-runner:v2 -f runner/Dockerfile runner
buildah push localhost/ra8-ci-runner:v2 \
  docker-archive:/tmp/ra8-ci-runner.tar:localhost/ra8-ci-runner:v2
sudo k3s ctr -n k8s.io images import /tmp/ra8-ci-runner.tar
```
