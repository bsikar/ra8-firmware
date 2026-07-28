# CI runner image = the pinned devcontainer toolchain + the GitHub Actions
# runner. ARC runner pods boot from this, so every gate executes in the exact
# environment `make ci` uses -- the toolchain drift that made gates pass
# locally yet fail in CI cannot recur.
#
# Two stages keep the toolchain defined in exactly one place:
#   - BASE_IMAGE (the devcontainer, built from ../../../.devcontainer/Dockerfile)
#     is the single source of truth for every pinned tool.
#   - RUNNER_IMAGE (the official actions-runner) supplies the runner binary and
#     the ARC k8s / k8s-novolume hooks verbatim, so the ARC contract is met
#     without hand-reimplementing it.
#
# Build (see ../README.md):
#   buildah bud -t localhost/ra8-ci-runner:latest -f Dockerfile .

# hadolint global ignore=DL3002
ARG BASE_IMAGE=localhost/ra8-devcontainer:latest
ARG RUNNER_IMAGE=ghcr.io/actions/actions-runner:latest

FROM ${RUNNER_IMAGE} AS runner

FROM ${BASE_IMAGE}

# The official runner image runs as uid 1001 ("runner"); the devcontainer's
# vscode user is uid 1000, so the two never clash. Every pinned tool lives in
# /usr/local, /usr/bin and /opt, so the runner user resolves them all.
#
# No sudoers entry (#487): every workflow step that used to provision packages
# on a bare-metal runner (e.g. the coverage gate's apt-get) was made
# unconditionally sudo-free in 5f4e8fd10, because the pod's toolchain is
# already complete and there is nothing left for such a step to install. A
# 'runner ALL=(ALL) NOPASSWD:ALL' line used to live here to match the official
# actions-runner image, but the deployed localhost/ra8-ci-runner:v2 image was
# built and imported without it (confirmed by exec into a live pod) and no job
# has needed it since -- the Dockerfile and the running image agreed on every
# other point, so the gap was pure drift, not a deliberate choice one of them
# made. Keeping the line here would have re-added a capability the fleet does
# not have and no gate exercises, the exact opposite of the honest-provisioning
# rule this file otherwise follows. Pods are sudo-less; keep it that way.
USER root
RUN groupadd -g 1001 runner \
    && useradd -m -u 1001 -g 1001 -G users -s /bin/bash runner

# Bring the runner and its ARC hooks over verbatim, then install the runner's
# own runtime dependencies (libicu, openssl, ...) into this base -- copying
# /home/runner alone does not carry the system libraries .NET needs.
COPY --from=runner --chown=runner:runner /home/runner /home/runner
RUN /home/runner/bin/installdependencies.sh \
    && rm -rf /var/lib/apt/lists/*

# The identity marker (#513). Its only job is to let a check running inside a
# container prove that the container is THIS image rather than some other
# Ubuntu that happens to carry the same tools.
#
# scripts/checks/check_runner_image_deps.py refuses to report a clean scan
# without it: the subject of that gate is the DEPLOYED image, and a probe that
# cannot show it reached one must fail rather than answer about whatever host
# it landed on. Deliberately content-free beyond the identity -- a build stamp
# here would make the layer uncacheable, and a copy of the tool pins would be a
# second place for them to drift from .devcontainer/Dockerfile.
RUN printf '%s\n' 'localhost/ra8-ci-runner (infra/images/runner/Dockerfile)' \
    > /etc/ra8-ci-runner

# Match the official image's runtime contract (no entrypoint, bash command, and
# the two env vars ARC relies on for signal handling and log streaming).
ENV RUNNER_MANUALLY_TRAP_SIG=1 \
    ACTIONS_RUNNER_PRINT_LOG_TO_STDOUT=1
WORKDIR /home/runner
USER runner
ENTRYPOINT []
CMD ["/bin/bash"]
