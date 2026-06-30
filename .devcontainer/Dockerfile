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

# Reproducibility posture (recon T5-02): the base image is pinned by its
# immutable index digest so `make ci` starts from a byte-identical root
# filesystem on every host. The toolchain packages the audit calls out --
# the ARM cross-compiler (whose version-specific miniz miscompile means
# codegen depends on the build host), its matching newlib, and the cppcheck /
# gcovr static-analysis + coverage tools -- are pinned to exact Ubuntu noble
# versions below. These four live in `universe` and do not rotate within an
# LTS, so the exact pin stays installable.
#
# The clang / llvm-18 family is deliberately NOT exact-version-pinned: it
# receives Ubuntu -security updates that rotate the exact version string off
# the mirror, so an exact pin (e.g. clang-18=1:18.1.3-1ubuntu1) would make the
# image UNBUILDABLE the moment a security update lands. It stays on the
# major-version package name (clang-18 / clang-tools-18 / llvm-18) plus the
# explicit clang-format-22 repo pin further down -- the same major the tree is
# formatted and linted against.
#
# Out-of-repo limit: the authoritative cross-compiler for the `build-cross` CI
# job is /opt/arm-gnu-toolchain-13.3 installed on the self-hosted runner (it
# bundles libstdc++, which the apt gcc-arm-none-eabi lacks). That toolchain is
# provisioned outside this repo and CANNOT be pinned here; full apt determinism
# would additionally require pointing apt at snapshot.ubuntu.com (follow-up).
FROM ubuntu:24.04@sha256:786a8b558f7be160c6c8c4a54f9a57274f3b4fb1491cf65146521ae77ff1dc54

ARG DEBIAN_FRONTEND=noninteractive

# Exact noble versions of the audit-flagged toolchain (see the posture note
# above). Bump these together when intentionally moving the pinned toolchain.
ARG ARM_GCC_VERSION=15:13.2.rel1-2
ARG ARM_NEWLIB_VERSION=4.4.0.20231231-2
ARG CPPCHECK_VERSION=2.13.0-2ubuntu3
ARG GCOVR_VERSION=7.0-1

RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential \
      ca-certificates \
      clang \
      clang-18 \
      clang-format \
      clang-tidy \
      cmake \
      cppcheck=${CPPCHECK_VERSION} \
      curl \
      doxygen \
      clang-tools-18 \
      gcc-arm-none-eabi=${ARM_GCC_VERSION} \
      gcovr=${GCOVR_VERSION} \
      git \
      graphviz \
      iwyu \
      libclang-rt-18-dev \
      libnewlib-arm-none-eabi=${ARM_NEWLIB_VERSION} \
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
