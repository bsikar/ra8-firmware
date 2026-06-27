#
# ra8d2-firmware development container.
#
# Ships an Ubuntu 24.04 base with the ARM GNU Toolchain, cmake,
# clang-format, clang-tidy, doxygen, and the J-Link toolchain
# prerequisites. Everything needed to build, test, and flash the
# firmware. SEGGER J-Link package is NOT baked in because it
# requires an EULA click-through -- install manually after first
# container start if you want `make flash` to work.
#

FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential \
      ca-certificates \
      clang \
      clang-18 \
      clang-format \
      clang-tidy \
      cmake \
      cppcheck \
      curl \
      doxygen \
      clang-tools-18 \
      gcc-arm-none-eabi \
      gcovr \
      git \
      graphviz \
      iwyu \
      libclang-rt-18-dev \
      libnewlib-arm-none-eabi \
      llvm-18 \
      ninja-build \
      pkg-config \
      python3 \
      python3-pip \
      sudo \
      unzip \
      wget \
    && rm -rf /var/lib/apt/lists/*

# Sanity-check that the clang-18 profile runtime is present. MC/DC
# instrumented binaries link against libclang_rt.profile-<arch>.a; if
# the package layout changes upstream we want the image build to fail
# loudly rather than at `make mcdc` time on a contributor's machine.
# The arch suffix is aarch64 on Apple Silicon hosts and x86_64 on
# Intel hosts -- accept either.
RUN set -eux; \
    rt_dir="/usr/lib/llvm-18/lib/clang/18/lib/linux"; \
    arch="$(uname -m)"; \
    test -f "${rt_dir}/libclang_rt.profile-${arch}.a" \
      || (echo "missing libclang_rt.profile-${arch}.a in ${rt_dir}" >&2; \
          ls "${rt_dir}" >&2; exit 1)

# git-lfs -- the repo LFS-tracks content/library/*.epub. `make ci` snapshots the
# tree with `git archive HEAD`, which spawns the git-lfs filter for those
# pointers; without the binary present, archive aborts mid-stream. No gate reads
# the epubs, so ci.sh runs archive under GIT_LFS_SKIP_SMUDGE=1 to emit the
# pointer files as-is (no LFS object or network fetch), but the binary must exist
# for the filter handshake to succeed.
RUN apt-get update \
    && apt-get install -y --no-install-recommends git-lfs \
    && rm -rf /var/lib/apt/lists/*

# clang-format-22 -- the exact binary the CI format gate pins
# (CLANG_FORMAT=clang-format-22 in .github/workflows/firmware.yml, and the tree
# is formatted to it). Ubuntu 24.04 only ships clang-format-18, which disagrees
# with v22 on edge cases, so `make ci`'s format gate would be unfaithful without
# it. Pulled from the official LLVM apt repository for noble. If LLVM 22 ever
# disappears from the snapshot mirror, scripts/ci.sh falls back to the highest
# clang-format-NN present and prints a clear warning rather than skipping.
RUN apt-get update \
    && apt-get install -y --no-install-recommends gnupg \
    && curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key \
         | gpg --dearmor -o /usr/share/keyrings/llvm-snapshot.gpg \
    && echo "deb [signed-by=/usr/share/keyrings/llvm-snapshot.gpg] http://apt.llvm.org/noble/ llvm-toolchain-noble-22 main" \
         > /etc/apt/sources.list.d/llvm-toolchain-noble-22.list \
    && apt-get update \
    && apt-get install -y --no-install-recommends clang-format-22 \
    && rm -rf /var/lib/apt/lists/*

# Create a non-root user so post-create scripts run as a human.
# Ubuntu 24.04 ships a pre-existing UID/GID 1000 (`ubuntu`); remove it
# first so we can claim the same id and have file ownership match the
# host bind-mount.
ARG USERNAME=vscode
ARG USER_UID=1000
ARG USER_GID=$USER_UID

RUN if id -u ubuntu >/dev/null 2>&1; then userdel -r ubuntu 2>/dev/null || true; fi \
    && groupadd --gid ${USER_GID} ${USERNAME} \
    && useradd --uid ${USER_UID} --gid ${USER_GID} -m ${USERNAME} \
    && echo "${USERNAME} ALL=(ALL) NOPASSWD:ALL" >> /etc/sudoers.d/${USERNAME}

USER ${USERNAME}
WORKDIR /workspace
