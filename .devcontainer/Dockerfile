#
# ra8-firmware development container.
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
# Cross-compiler: this image fetches Arm GNU Toolchain 13.3.rel1 to
# /opt/arm-gnu-toolchain-13.3 by URL + sha256 (below) -- the SAME release + path
# the self-hosted runner's `build-cross` job uses, so `make ci` and CI build
# byte-reproducible codegen and cmake/toolchain-ra8d2.cmake's HINTS find it. It
# bundles libstdc++ (the apt gcc-arm-none-eabi did not, so the C++ apps could not
# build here) and its own newlib, so neither apt package is installed anymore.
FROM ubuntu:24.04@sha256:786a8b558f7be160c6c8c4a54f9a57274f3b4fb1491cf65146521ae77ff1dc54

ARG DEBIAN_FRONTEND=noninteractive

# Exact noble versions of the audit-flagged toolchain (see the posture note
# above). Bump these together when intentionally moving the pinned toolchain.
ARG CPPCHECK_VERSION=2.13.0-2ubuntu3
ARG GCOVR_VERSION=7.0-1

# Arm GNU Toolchain 13.3.rel1 -- pinned by URL + sha256 so the devcontainer's
# cross-compiler is byte-identical to the runner's /opt/arm-gnu-toolchain-13.3
# (#178). Bump the release and BOTH per-arch sha256 together to move the pin.
ARG ARM_GCC_RELEASE=13.3.rel1
ARG ARM_GCC_SHA256_X86_64=95c011cee430e64dd6087c75c800f04b9c49832cc1000127a92a97f9c8d83af4
ARG ARM_GCC_SHA256_AARCH64=c8824bffd057afce2259f7618254e840715f33523a3d4e4294f471208f976764

# libunicorn-dev / libcapstone-dev are the Unicorn + Capstone headers that
# tools/board_sim includes, and libcurl4-openssl-dev is the libcurl header
# tools/media_dl's network backend includes. `make ci` lints tools/ (issue
# #296), so the tidy gate needs those headers present inside the container even
# though the container builds neither tool itself. A missing header does not
# fail loudly -- clang-tidy emits clang-diagnostic-error and never lints that
# TU -- so keep this list in step with the runner install step in
# .github/workflows/firmware.yml and the pkg-config probe in
# scripts/clang_tidy.sh.
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
      gcovr=${GCOVR_VERSION} \
      git \
      graphviz \
      iwyu \
      libcapstone-dev \
      libclang-rt-18-dev \
      libcurl4-openssl-dev \
      libunicorn-dev \
      llvm-18 \
      locales \
      ninja-build \
      pkg-config \
      python3 \
      python3-pip \
      sudo \
      tree \
      unzip \
      wget \
      xz-utils \
      zsh \
    && rm -rf /var/lib/apt/lists/* \
    && sed -i '/en_US.UTF-8/s/^# //' /etc/locale.gen \
    && locale-gen

# UTF-8 locale so oh-my-zsh / powerlevel10k render glyphs and box-drawing.
ENV LANG=en_US.UTF-8 \
    LC_ALL=en_US.UTF-8

# Arm GNU Toolchain 13.3.rel1 -> /opt/arm-gnu-toolchain-13.3 (the runner's path,
# found by cmake/toolchain-ra8d2.cmake's HINTS). Multi-arch: this image is built
# amd64 (x86 runner / `make ci`) or arm64 (Apple-Silicon `make ci`); pick + verify
# the matching tarball by sha256 so a corrupted or swapped download fails loudly.
RUN set -eux; \
    case "$(uname -m)" in \
      x86_64)  tarch=x86_64;  sha="${ARM_GCC_SHA256_X86_64}" ;; \
      aarch64) tarch=aarch64; sha="${ARM_GCC_SHA256_AARCH64}" ;; \
      *) echo "unsupported host arch $(uname -m) for arm-gnu-toolchain" >&2; exit 1 ;; \
    esac; \
    url="https://developer.arm.com/-/media/Files/downloads/gnu/${ARM_GCC_RELEASE}/binrel/arm-gnu-toolchain-${ARM_GCC_RELEASE}-${tarch}-arm-none-eabi.tar.xz"; \
    curl -fsSL "$url" -o /tmp/armgcc.tar.xz; \
    echo "${sha}  /tmp/armgcc.tar.xz" | sha256sum -c -; \
    mkdir -p /opt/arm-gnu-toolchain-13.3; \
    tar xf /tmp/armgcc.tar.xz -C /opt/arm-gnu-toolchain-13.3 --strip-components=1; \
    rm /tmp/armgcc.tar.xz; \
    /opt/arm-gnu-toolchain-13.3/bin/arm-none-eabi-gcc -dumpfullversion
ENV PATH="/opt/arm-gnu-toolchain-13.3/bin:${PATH}"

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

# libclang Python bindings -- what scripts/utils/check_annotations.py walks the
# AST with. Pinned to the same version firmware.yml installs for its
# "Annotation-attribute gate (libclang)" step so `make ci` sees the same AST as
# CI. The wheel bundles its own libclang.so, so this is independent of the
# clang-18 packages above. Installed system-wide with --break-system-packages
# because Ubuntu 24.04 marks the system interpreter PEP 668 externally-managed;
# the container is disposable and single-purpose, so a venv would only add a
# PATH indirection every gate would have to know about.
#
# Without this the annotation gate cannot run, and a checker that cannot run is
# worse than one that is absent: it exits 0 and reports nothing. ci.sh therefore
# probes the import and fails loudly rather than skipping.
ARG LIBCLANG_VERSION=18.1.1
RUN python3 -m pip install --no-cache-dir --break-system-packages \
      libclang==${LIBCLANG_VERSION} \
    && python3 -c "import clang.cindex"

# ccache -- the compiler cache cmake/ccache.cmake wires in as
# CMAKE_<LANG>_COMPILER_LAUNCHER. It must exist INSIDE the image because every
# gate build runs in this container against a throwaway snapshot tree; without
# it each `make ci` recompiles the whole tree from scratch, which on the shared
# verification box means several agents doing that simultaneously on 12 cores.
#
# The cache itself is a bind-mounted host directory (scripts/ci.sh mounts it at
# /ccache and sets CCACHE_DIR), so hits survive the container being --rm'd and
# are shared across agents. CCACHE_BASEDIR + CCACHE_NOHASHDIR, also set by
# ci.sh, are what let a snapshot at a fresh mktemp path still hit entries
# produced under a different path.
#
# Deliberately a late layer: appending it here leaves the expensive apt and
# arm-gnu-toolchain layers above cached on rebuild.
RUN apt-get update \
    && apt-get install -y --no-install-recommends ccache \
    && rm -rf /var/lib/apt/lists/* \
    && ccache --version

# Create a non-root user so post-create scripts run as a human.
# Ubuntu 24.04 ships a pre-existing UID/GID 1000 (`ubuntu`); remove it
# first so we can claim the same id and have file ownership match the
# host bind-mount.
ARG USERNAME=vscode
ARG USER_UID=1000
ARG USER_GID=$USER_UID

RUN if id -u ubuntu >/dev/null 2>&1; then userdel -r ubuntu 2>/dev/null || true; fi \
    && groupadd --gid ${USER_GID} ${USERNAME} \
    && useradd --uid ${USER_UID} --gid ${USER_GID} -m -s /usr/bin/zsh ${USERNAME} \
    && echo "${USERNAME} ALL=(ALL) NOPASSWD:ALL" >> /etc/sudoers.d/${USERNAME}

USER ${USERNAME}
WORKDIR /workspace

# ---------------------------------------------------------------------------
# Shell: zsh + oh-my-zsh + powerlevel10k (matches the maintainer's local
# setup). The container defaulted to a bare `sh`, which is a poor interactive
# experience; this makes the terminal a first-class dev shell -- git-aware
# prompt, autosuggestions, and syntax highlighting. Installed into the vscode
# user's $HOME. The login shell is set separately via `useradd -s`, and the
# .zshrc is the committed one copied below, so no interactive installer is
# needed -- everything is a plain, pinned git clone.
#
# NOTE: powerlevel10k's icons need a Nerd Font in the *client* terminal, which
# is a VS Code / host-side setting we cannot install from the image. Set
# `"terminal.integrated.fontFamily": "MesloLGS NF"` (and install that font on
# your machine) for the full prompt; without it the shell still works, just
# with fallback glyphs.
# ---------------------------------------------------------------------------
# Pinned to immutable refs so a rebuild is reproducible and cannot drift when
# these upstreams move (same discipline as the ARM_GCC_SHA256 / *_VERSION pins
# above). oh-my-zsh publishes no release tags, so it is pinned by commit; the
# rest use release tags. Bump deliberately, never implicitly. A plain
# `git clone` at a pinned ref also avoids piping a live installer script into a
# shell (`curl | sh`).
ARG OMZ_COMMIT=677a4592b18c08ddea737f8aca70bac0e9fc9313
ARG P10K_TAG=v1.20.0
ARG ZSH_AUTOSUGGESTIONS_TAG=v0.7.1
ARG ZSH_SYNTAX_HIGHLIGHTING_TAG=0.8.0
RUN set -eux; \
    export HOME="/home/${USERNAME}"; \
    ZSH="${HOME}/.oh-my-zsh"; \
    ZSH_CUSTOM="${ZSH}/custom"; \
    git clone https://github.com/ohmyzsh/ohmyzsh.git "${ZSH}"; \
    git -C "${ZSH}" checkout --quiet "${OMZ_COMMIT}"; \
    git clone --branch "${P10K_TAG}" --depth=1 https://github.com/romkatv/powerlevel10k.git \
        "${ZSH_CUSTOM}/themes/powerlevel10k"; \
    git clone --branch "${ZSH_AUTOSUGGESTIONS_TAG}" --depth=1 \
        https://github.com/zsh-users/zsh-autosuggestions.git \
        "${ZSH_CUSTOM}/plugins/zsh-autosuggestions"; \
    git clone --branch "${ZSH_SYNTAX_HIGHLIGHTING_TAG}" --depth=1 \
        https://github.com/zsh-users/zsh-syntax-highlighting.git \
        "${ZSH_CUSTOM}/plugins/zsh-syntax-highlighting"

COPY --chown=${USERNAME}:${USERNAME} zshrc    /home/${USERNAME}/.zshrc
COPY --chown=${USERNAME}:${USERNAME} p10k.zsh /home/${USERNAME}/.p10k.zsh

# Git config for the workspace. `feature.manyFiles` (index v4 + untracked cache)
# speeds up `git status` on this large tree, which is otherwise slow over the
# bind-mounted workspace on non-Linux hosts. `safe.directory` preempts git's
# "detected dubious ownership" refusal when the bind mount's uid does not match
# the container user. This is only a partial remedy: the real fix for slow git
# on a Windows/macOS host is to keep the checkout in a container volume rather
# than a host bind mount (VS Code: "Dev Containers: Clone Repository in Container
# Volume"). See the workspaceMount note in devcontainer.json.
RUN git config --global feature.manyFiles true \
    && git config --global --add safe.directory /workspace
