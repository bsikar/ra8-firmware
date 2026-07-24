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
# /usr/local, /usr/bin and /opt, so the runner user resolves them all. The
# runner also gets passwordless sudo, matching the official image: workflow
# steps that provision packages on a bare-metal runner (e.g. the coverage
# gate's apt-get) then run as harmless no-ops in the pod, whose toolchain is
# already complete, instead of failing on a password prompt with no terminal.
USER root
RUN groupadd -g 1001 runner \
    && useradd -m -u 1001 -g 1001 -G sudo,users -s /bin/bash runner \
    && echo 'runner ALL=(ALL) NOPASSWD:ALL' > /etc/sudoers.d/runner \
    && chmod 0440 /etc/sudoers.d/runner

# Bring the runner and its ARC hooks over verbatim, then install the runner's
# own runtime dependencies (libicu, openssl, ...) into this base -- copying
# /home/runner alone does not carry the system libraries .NET needs.
COPY --from=runner --chown=runner:runner /home/runner /home/runner
RUN /home/runner/bin/installdependencies.sh \
    && rm -rf /var/lib/apt/lists/*

# Match the official image's runtime contract (no entrypoint, bash command, and
# the two env vars ARC relies on for signal handling and log streaming).
ENV RUNNER_MANUALLY_TRAP_SIG=1 \
    ACTIONS_RUNNER_PRINT_LOG_TO_STDOUT=1
WORKDIR /home/runner
USER runner
ENTRYPOINT []
CMD ["/bin/bash"]
