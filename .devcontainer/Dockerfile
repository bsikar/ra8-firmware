#
# ra8-firmware development container.
#
# Ships an Ubuntu 24.04 base with the ARM GNU Toolchain, cmake,
# clang-format, clang-tidy, doxygen, and the J-Link toolchain
# prerequisites. Everything needed to build, test, and flash the
# firmware. SEGGER J-Link package is NOT baked in because it
# requires an EULA click-through -- install manually after first
# container start if you want local J-Link flash recipes to work.
#

# Reproducibility posture (recon T5-02): the base image is pinned by its
# immutable index digest so `just ci` starts from a byte-identical root
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
# gcc-14 is the SECOND host-tool compiler arm (#356). The tools-build gate
# compiles the first-party host tools under clang-18 AND gcc-14 so the warning
# families each catches but the other misses are both held: gcc-14's
# -Wformat-truncation found a silent PATH_MAX path-join truncation in
# apps/host/mdl that clang-18 did not flag. Like the clang family it tracks the
# major-version package name (gcc-14) rather than an exact string, and
# check_tool_versions.py reads that major from here.
#
# Cross-compiler: this image fetches Arm GNU Toolchain 13.3.rel1 to
# /opt/arm-gnu-toolchain-13.3 by URL + sha256 (below) -- the SAME release + path
# the self-hosted runner's `build-cross` job uses, so `just ci` and CI build
# byte-reproducible codegen and cmake/toolchain-ra8d2.cmake's HINTS find it. It
# bundles libstdc++ (the apt gcc-arm-none-eabi did not, so the C++ apps could not
# build here) and its own newlib, so neither apt package is installed anymore.
FROM ubuntu:24.04@sha256:786a8b558f7be160c6c8c4a54f9a57274f3b4fb1491cf65146521ae77ff1dc54

ARG DEBIAN_FRONTEND=noninteractive

# Every RUN below runs under bash with pipefail (hadolint DL4006).
#
# This is not a style preference. Docker defaults RUN to `/bin/sh -c`, where a
# pipeline reports only the LAST command's status -- so the eight
# `curl -fsSL <url> | tar -x` lines in this file would report success when the
# download failed and the tarball was empty, silently producing an image whose
# pinned tool is simply absent. That is the same failure this tree already
# gates against in shell (scripts/checks/check_errexit_masking.py) and the same
# reason `just ci` must fail rather than skip on a missing tool.
SHELL ["/bin/bash", "-o", "pipefail", "-c"]

# Exact noble version of the audit-flagged toolchain (see the posture note
# above). Bump it deliberately when intentionally moving the pinned toolchain.
ARG CPPCHECK_VERSION=2.13.0-2ubuntu3

# Arm GNU Toolchain 13.3.rel1 -- pinned by URL + sha256 so the devcontainer's
# cross-compiler is byte-identical to the runner's /opt/arm-gnu-toolchain-13.3
# (#178). Linux and Darwin publish different archives for each architecture;
# bump the release and all four per-platform sha256 pins together.
ARG ARM_GCC_RELEASE=13.3.rel1
ARG ARM_GCC_SHA256_X86_64=95c011cee430e64dd6087c75c800f04b9c49832cc1000127a92a97f9c8d83af4
ARG ARM_GCC_SHA256_AARCH64=c8824bffd057afce2259f7618254e840715f33523a3d4e4294f471208f976764
ARG ARM_GCC_SHA256_DARWIN_X86_64=1ab00742d1ed0926e6f227df39d767f8efab46f5250505c29cb81f548222d794
ARG ARM_GCC_SHA256_DARWIN_ARM64=fb6921db95d345dc7e5e487dd43b745e3a5b4d5c0c7ca4f707347148760317b4

# Unicorn CPU emulator -- pinned by URL + sha256 (built from source below) so
# ra8_emulator decodes Armv8.1-M (Helium/MVE) IDENTICALLY to the self-hosted
# runner. An unpinned Unicorn is exactly the #354 skew (runner 2.1.4 vs an old
# 2.0.1). Keep these two in step with RA8_UNICORN_* in scripts/ci/unicorn_pin.sh.
ARG UNICORN_VERSION=2.1.4
ARG UNICORN_SHA256=ea8863f095a0136388694e5a6063afd9bb7650e30243dd6251af59c5ce5601f4

# doxygen -- pinned by URL + sha256 so the `docs` gate's doxygen matches the
# release docs/doxygen_theme/header.html and the vendored doxygen-awesome
# theme were generated against (#486). Ubuntu's apt package is 1.9.8, which
# leaves the templates rendering with unexpanded placeholders like
# $mermaidjs; scripts/builders/provision_doxygen.sh already pins the same
# 1.16.1 release + sha256 for a bare host run, and used to have to download it
# on every single ra8-ci pod (the runner is ephemeral -- one pod per job --
# and had no writable persistent cache either, see infra/ansible/roles/
# ci_runner). Baking the exact release in here means `command -v doxygen`
# resolves it directly and provision_doxygen.sh never touches the network.
# Bump this value and DOXYGEN_SHA256_LINUX_X64 together with PINNED_VERSION /
# SHA256_LINUX_X64 in provision_doxygen.sh -- one release, cited twice.
ARG DOXYGEN_VERSION=1.16.1
ARG DOXYGEN_SHA256_LINUX_X64=a56f885d37e3aae08a99f638d17bbb381224c03a878d9e2dda4f9fa4baf1d8bd

# libcapstone-dev is the Capstone header that tools/ra8_emulator includes (for
# disassembly only -- NOT the decode variable), and libcurl4-openssl-dev is the
# libcurl header apps/host/mdl's network backend includes. `just ci` lints
# tools/ (issue #296), so the tidy gate needs those headers present inside the
# container even though the tidy pass builds neither tool. A missing header does
# not fail loudly -- clang-tidy emits clang-diagnostic-error and never lints
# that TU -- so keep this list in step with the runner install step in
# .github/workflows/firmware.yml. Unicorn is NOT here: it is the pinned source
# build below (docs/TOOLCHAIN.md), not an apt package.
#
# THE INVARIANT (#513): this list is a SUPERSET of every tool the gates declare
# with require_cmd / require_python_mod. The `runner-image-deps` gate asserts it
# against the DEPLOYED image rather than against this file, because the two are
# free to disagree and have. openssh-client is here for exactly that reason and
# no other: gate_bench_lock_selftest declares `ssh` and the image carried none
# -- the third case of "a gate asserts a tool the image lacks" inside two days,
# after `gh` and `jq`, and the first one found by a check instead of a person.
#
# openssl is the fourth, and it arrived through a route runner-image-deps
# cannot see: apps/host/mdl/CMakeLists.txt find_program()s the openssl CLI as
# REQUIRED for its HTTPS integration test, so the tools-build gate depended on
# a binary no require_cmd declared and this list never carried. Every developer
# box has it; ubuntu:24.04 plus the list above does not, so the gate failed at
# `cmake -S apps/host/mdl` on the runner and nowhere else. A CMake
# find_program(... REQUIRED) is a host-tool declaration too -- if you add one,
# add the package here.
#
# python3-venv supplies ensurepip for managed_python_env.py's stdlib-only
# authentication selftest. Ubuntu deliberately splits it from python3; without
# it, the toolchain-parity gate cannot prove that copied or stale virtual
# environments are rejected.
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential \
      ca-certificates \
      clang \
      clang-18 \
      clang-format \
      clang-tidy \
      clangd-18 \
      cmake \
      cppcheck=${CPPCHECK_VERSION} \
      curl \
      doxygen \
      clang-tools-18 \
      gcc-14 \
      g++-14 \
      git \
      graphviz \
      iwyu \
      jq \
      libcapstone-dev \
      libclang-rt-18-dev \
      libcurl4-openssl-dev \
      llvm-18 \
      locales \
      ninja-build \
      openssh-client \
      openssl \
      pkg-config \
      poppler-utils \
      python3 \
      python3-venv \
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
# amd64 (x86 runner / `just ci`) or arm64 (Apple-Silicon `just ci`); pick + verify
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

# Unicorn ${UNICORN_VERSION} from source -> /usr/local (the runner's layout), so
# `just ci`'s ra8_emulator gates decode Armv8.1-M byte-identically to CI. Pinned by
# URL + sha256 so a corrupted/swapped/regenerated download fails loudly. Only
# the arm target is built (ra8_emulator emulates the Cortex-M core), which keeps
# the build small without changing the arm decode. Verified by re-reading the
# installed header's UC_API_* version afterwards.
RUN set -eux; \
    url="https://github.com/unicorn-engine/unicorn/archive/refs/tags/${UNICORN_VERSION}.tar.gz"; \
    curl -fsSL "$url" -o /tmp/unicorn.tar.gz; \
    echo "${UNICORN_SHA256}  /tmp/unicorn.tar.gz" | sha256sum -c -; \
    tar -xzf /tmp/unicorn.tar.gz -C /tmp; \
    cmake -S "/tmp/unicorn-${UNICORN_VERSION}" -B /tmp/unicorn-build \
      -DCMAKE_BUILD_TYPE=Release -DUNICORN_ARCH=arm -DBUILD_SHARED_LIBS=ON \
      -DCMAKE_INSTALL_PREFIX=/usr/local; \
    cmake --build /tmp/unicorn-build --parallel "$(nproc)"; \
    cmake --install /tmp/unicorn-build; \
    ldconfig; \
    rm -rf /tmp/unicorn.tar.gz "/tmp/unicorn-${UNICORN_VERSION}" /tmp/unicorn-build; \
    grep -E 'UC_API_(MAJOR|MINOR|PATCH)' /usr/local/include/unicorn/unicorn.h

# doxygen ${DOXYGEN_VERSION} official Linux binary -> /usr/local/bin (#486).
# Only x86_64: doxygen publishes no official linux-arm64 binary, so an
# Apple-Silicon-hosted `just ci` container (this same Dockerfile, built
# natively arm64) keeps the unpinned apt doxygen installed above -- exactly
# today's behaviour, not a regression. /usr/local/bin sorts before /usr/bin on
# $PATH, so this shadows the apt package's `doxygen` on x86_64 without having
# to uninstall it. The real ra8-ci runner node is x86_64 (confirmed via
# `uname -m` on the k3s host), so this is the build that actually matters for
# #486.
RUN set -eux; \
    if [ "$(uname -m)" = "x86_64" ]; then \
      url="https://github.com/doxygen/doxygen/releases/download/Release_$(echo "${DOXYGEN_VERSION}" | tr . _)/doxygen-${DOXYGEN_VERSION}.linux.bin.tar.gz"; \
      curl -fsSL "$url" -o /tmp/doxygen.tar.gz; \
      echo "${DOXYGEN_SHA256_LINUX_X64}  /tmp/doxygen.tar.gz" | sha256sum -c -; \
      tar -xzf /tmp/doxygen.tar.gz -C /tmp; \
      install -m 0755 "/tmp/doxygen-${DOXYGEN_VERSION}/bin/doxygen" /usr/local/bin/doxygen; \
      rm -rf /tmp/doxygen.tar.gz "/tmp/doxygen-${DOXYGEN_VERSION}"; \
      test "$(doxygen --version | awk '{print $1}')" = "${DOXYGEN_VERSION}"; \
    else \
      echo "doxygen: skipping the pinned x86_64 binary on $(uname -m); apt's doxygen stays unpinned here (#486)"; \
    fi

# Sanity-check that the clang-18 profile runtime is present. MC/DC
# instrumented binaries link against libclang_rt.profile-<arch>.a; if
# the package layout changes upstream we want the image build to fail
# loudly rather than during `just quality::local::mcdc` on a contributor's machine.
# The arch suffix is aarch64 on Apple Silicon hosts and x86_64 on
# Intel hosts -- accept either.
RUN set -eux; \
    rt_dir="/usr/lib/llvm-18/lib/clang/18/lib/linux"; \
    arch="$(uname -m)"; \
    test -f "${rt_dir}/libclang_rt.profile-${arch}.a" \
      || (echo "missing libclang_rt.profile-${arch}.a in ${rt_dir}" >&2; \
          ls "${rt_dir}" >&2; exit 1)

# git-lfs -- the repo LFS-tracks apps/board/stand_alone/ereader/content/library/*.epub. `just ci` snapshots the
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
# with v22 on edge cases, so `just ci`'s format gate would be unfaithful without
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

# Python-managed gate dependencies live in one isolated environment. Their
# direct and transitive versions come only from pyproject.toml and uv.lock. uv
# itself is downloaded by the standard-library bootstrap from the one pinned,
# checksum-verified release manifest. The retained archive under /opt lets the
# bootstrap reauthenticate the extracted executable on every future reuse.
#
# The CI group includes runtime imports plus every Python gate CLI. Syncing is
# exact, refuses lock changes, refuses Python downloads, and never mutates the
# PEP 668 distribution interpreter. RA8_TOOL_VENV makes the managed environment
# authoritative and excludes a host .venv bind-mounted at /workspace.
ARG PYTHON_TOOL_VENV=/opt/ra8-python-tools
COPY pyproject.toml uv.lock /opt/ra8-python-project/
COPY scripts/dev/bootstrap_uv.py \
     scripts/dev/bootstrap_uv_exec.py \
     scripts/dev/managed_python_env.py \
     scripts/dev/managed_python_env_checks.py \
     scripts/dev/uv_release.json \
     /opt/ra8-uv-bootstrap/
WORKDIR /opt/ra8-python-project
RUN set -eux; \
    UV_PROJECT_ENVIRONMENT="${PYTHON_TOOL_VENV}" UV_PYTHON_DOWNLOADS=never \
      /usr/bin/python3 -I -S /opt/ra8-uv-bootstrap/bootstrap_uv.py \
      --cache-root /opt/ra8-uv-cache --ensure-and-run --no-config sync \
      --locked --only-group ci \
      --no-install-project --python /usr/bin/python3; \
    UV_PYTHON_DOWNLOADS=never \
      /usr/bin/python3 -I -S /opt/ra8-uv-bootstrap/bootstrap_uv.py \
      --cache-root /opt/ra8-uv-cache --run --no-config lock --check; \
    UV_PYTHON_DOWNLOADS=never \
      /usr/bin/python3 -I -S /opt/ra8-uv-bootstrap/bootstrap_uv.py \
      --cache-root /opt/ra8-uv-cache --run --no-config pip check \
      --python "${PYTHON_TOOL_VENV}/bin/python3"; \
    "${PYTHON_TOOL_VENV}/bin/python3" -c "import PIL, clang.cindex, dotenv, kasa, serial, usb.core, yaml"; \
    "${PYTHON_TOOL_VENV}/bin/ruff" --version; \
    "${PYTHON_TOOL_VENV}/bin/cmake-format" --version; \
    "${PYTHON_TOOL_VENV}/bin/yamllint" --version; \
    "${PYTHON_TOOL_VENV}/bin/gcovr" --version; \
    rm -f -- "${PYTHON_TOOL_VENV}/.lock"
ENV RA8_TOOL_VENV="${PYTHON_TOOL_VENV}" \
    RA8_UV_CACHE_ROOT="/opt/ra8-uv-cache" \
    VIRTUAL_ENV="${PYTHON_TOOL_VENV}" \
    UV_PYTHON_DOWNLOADS="never" \
    PATH="${PYTHON_TOOL_VENV}/bin:${PATH}"

# ccache -- the compiler cache cmake/ccache.cmake wires in as
# CMAKE_<LANG>_COMPILER_LAUNCHER. It must exist INSIDE the image because every
# gate build runs in this container against a throwaway snapshot tree; without
# it each `just ci` recompiles the whole tree from scratch, which on the shared
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

# ruff / shellcheck / shfmt -- the three tools the `lint-py-shell` gate needs.
#
# Without them that gate could never pass in this container. It does not skip
# (check_ruff.py / check_shell.py are invoked with --require, and a gate whose
# dependency is absent must FAIL) -- so the containerised `just ci`, which is
# the documented macOS path, reported FAIL on every run no matter what the tree
# contained. CI itself was green because the self-hosted Linux runners have
# these installed natively, which is exactly the local-green/CI-red divergence
# in reverse and just as misleading.
#
# Versions are pinned to what CI resolves, so the gate cannot disagree with the
# runner about a lint rule. Keep these in step with docs/TOOLCHAIN.md section
# 3.4 and with whatever the runners install.
ARG SHELLCHECK_VERSION=0.11.0
ARG SHELLCHECK_SHA256_X86_64=8c3be12b05d5c177a04c29e3c78ce89ac86f1595681cab149b65b97c4e227198
ARG SHELLCHECK_SHA256_AARCH64=12b331c1d2db6b9eb13cfca64306b1b157a86eb69db83023e261eaa7e7c14588
ARG SHFMT_VERSION=3.13.1
ARG SHFMT_SHA256_AMD64=fb096c5d1ac6beabbdbaa2874d025badb03ee07929f0c9ff67563ce8c75398b1
ARG SHFMT_SHA256_ARM64=32d92acaa5cd8abb29fc49dac123dc412442d5713967819d8af2c29f1b3857c7
RUN set -eux; \
    case "$(uname -m)" in \
      x86_64)  scarch=x86_64; sharch=amd64; scsha="${SHELLCHECK_SHA256_X86_64}"; shsha="${SHFMT_SHA256_AMD64}" ;; \
      aarch64) scarch=aarch64; sharch=arm64; scsha="${SHELLCHECK_SHA256_AARCH64}"; shsha="${SHFMT_SHA256_ARM64}" ;; \
      *) echo "unsupported host arch $(uname -m) for shellcheck/shfmt" >&2; exit 1 ;; \
    esac; \
    curl --proto '=https' --proto-redir '=https' --tlsv1.2 -fsSL -o /tmp/shellcheck.tar.xz \
      "https://github.com/koalaman/shellcheck/releases/download/v${SHELLCHECK_VERSION}/shellcheck-v${SHELLCHECK_VERSION}.linux.${scarch}.tar.xz"; \
    printf '%s  %s\n' "${scsha}" /tmp/shellcheck.tar.xz | sha256sum -c -; \
    mkdir /tmp/shellcheck; \
    tar -xJf /tmp/shellcheck.tar.xz -C /tmp/shellcheck --strip-components=1 \
      "shellcheck-v${SHELLCHECK_VERSION}/shellcheck"; \
    install -m 0755 /tmp/shellcheck/shellcheck /usr/local/bin/shellcheck; \
    curl --proto '=https' --proto-redir '=https' --tlsv1.2 -fsSL -o /tmp/shfmt \
      "https://github.com/mvdan/sh/releases/download/v${SHFMT_VERSION}/shfmt_v${SHFMT_VERSION}_linux_${sharch}"; \
    printf '%s  %s\n' "${shsha}" /tmp/shfmt | sha256sum -c -; \
    install -m 0755 /tmp/shfmt /usr/local/bin/shfmt; \
    rm -rf /tmp/shellcheck /tmp/shellcheck.tar.xz /tmp/shfmt; \
    ruff --version; shellcheck --version | grep '^version:'; shfmt --version

# cmake-format / cmake-lint / yamllint / actionlint -- the tools the lint-cmake
# and lint-yaml gates need. Same reasoning as the block above: both gates call
# require_cmd and FAIL rather than skip, so without these the containerised
# `just ci` could never pass while CI stayed green on the self-hosted runners
# that have them natively.
#
# Versions are pinned to what the runners install (see the "Install the
# config-as-code linters" step in .github/workflows/firmware.yml). Keep the
# three in step.
ARG ACTIONLINT_VERSION=1.7.7
ARG ACTIONLINT_SHA256_AMD64=023070a287cd8cccd71515fedc843f1985bf96c436b7effaecce67290e7e0757
ARG ACTIONLINT_SHA256_ARM64=401942f9c24ed71e4fe71b76c7d638f66d8633575c4016efd2977ce7c28317d0
RUN set -eux; \
    case "$(uname -m)" in \
      x86_64)  alarch=amd64; alsha="${ACTIONLINT_SHA256_AMD64}" ;; \
      aarch64) alarch=arm64; alsha="${ACTIONLINT_SHA256_ARM64}" ;; \
      *) echo "unsupported host arch $(uname -m) for actionlint" >&2; exit 1 ;; \
    esac; \
    curl --proto '=https' --proto-redir '=https' --tlsv1.2 -fsSL -o /tmp/actionlint.tar.gz \
      "https://github.com/rhysd/actionlint/releases/download/v${ACTIONLINT_VERSION}/actionlint_${ACTIONLINT_VERSION}_linux_${alarch}.tar.gz"; \
    printf '%s  %s\n' "${alsha}" /tmp/actionlint.tar.gz | sha256sum -c -; \
    mkdir /tmp/actionlint; \
    tar -xzf /tmp/actionlint.tar.gz -C /tmp/actionlint actionlint; \
    install -m 0755 /tmp/actionlint/actionlint /usr/local/bin/actionlint; \
    rm -rf /tmp/actionlint /tmp/actionlint.tar.gz; \
    cmake-format --version; cmake-lint --version; \
    yamllint --version; actionlint --version

# just -- command runner for CI gates and repository recipes.
ARG JUST_VERSION=1.40.0
ARG JUST_SHA256_X86_64=181b91d0ceebe8a57723fb648ed2ce1a44d849438ce2e658339df4f8db5f1263
ARG JUST_SHA256_AARCH64=d065d0df1a1f99529869fba8a5b3e0a25c1795b9007099b00dfabe29c7c1f7b6
RUN set -eux; \
    case "$(uname -m)" in \
      x86_64)  justarch=x86_64; justsha="${JUST_SHA256_X86_64}" ;; \
      aarch64) justarch=aarch64; justsha="${JUST_SHA256_AARCH64}" ;; \
      *) echo "unsupported host arch $(uname -m) for just" >&2; exit 1 ;; \
    esac; \
    curl --proto '=https' --proto-redir '=https' --tlsv1.2 -fsSL -o /tmp/just.tar.gz \
      "https://github.com/casey/just/releases/download/${JUST_VERSION}/just-${JUST_VERSION}-${justarch}-unknown-linux-musl.tar.gz"; \
    printf '%s  %s\n' "${justsha}" /tmp/just.tar.gz | sha256sum -c -; \
    mkdir /tmp/just; \
    tar -xzf /tmp/just.tar.gz -C /tmp/just just; \
    install -m 0755 /tmp/just/just /usr/local/bin/just; \
    rm -rf /tmp/just /tmp/just.tar.gz; \
    just --version

# hadolint -- the tool the `lint-devcontainer` gate needs to lint THIS file.
#
# Same reasoning as the two blocks above: the gate calls require_cmd and FAILS
# rather than skipping, so without this the containerised `just ci` could never
# pass. Pinned to what the runner installs (see the "Install hadolint" step in
# .github/workflows/firmware.yml); keep the two in step. Rule configuration
# lives in .hadolint.yaml at the repo root.
#
# zsh needs no block here: it is already installed above for the interactive
# shell, and `zsh -n` is all the zshrc check requires.
ARG HADOLINT_VERSION=2.14.0
ARG HADOLINT_SHA256_X86_64=6bf226944684f56c84dd014e8b979d27425c0148f61b3bd99bcc6f39e9dc5a47
ARG HADOLINT_SHA256_ARM64=331f1d3511b84a4f1e3d18d52fec284723e4019552f4f47b19322a53ce9a40ed
RUN set -eux; \
    case "$(uname -m)" in \
      x86_64)  harch=x86_64; hsha="${HADOLINT_SHA256_X86_64}" ;; \
      aarch64) harch=arm64; hsha="${HADOLINT_SHA256_ARM64}" ;; \
      *) echo "unsupported host arch $(uname -m) for hadolint" >&2; exit 1 ;; \
    esac; \
    curl --proto '=https' --proto-redir '=https' --tlsv1.2 -fsSL -o /tmp/hadolint \
      "https://github.com/hadolint/hadolint/releases/download/v${HADOLINT_VERSION}/hadolint-linux-${harch}"; \
    printf '%s  %s\n' "${hsha}" /tmp/hadolint | sha256sum -c -; \
    install -m 0755 /tmp/hadolint /usr/local/bin/hadolint; \
    rm -f /tmp/hadolint; \
    hadolint --version

# Create a non-root user so post-create scripts run as a human.
# Ubuntu 24.04 ships a pre-existing UID/GID 1000 (`ubuntu`); remove it
# first so we can claim the same id and have file ownership match the
# host bind-mount.
ARG USERNAME=vscode
ARG USER_UID=1000
ARG USER_GID=$USER_UID

RUN if id -u ubuntu >/dev/null 2>&1; then userdel -r ubuntu; fi \
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

COPY --chown=${USERNAME}:${USERNAME} .devcontainer/zshrc    /home/${USERNAME}/.zshrc
COPY --chown=${USERNAME}:${USERNAME} .devcontainer/p10k.zsh /home/${USERNAME}/.p10k.zsh

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

# Seal only after every image-build command has finished. Keeping this as the
# final RUN instruction makes later managed-interpreter use structurally
# impossible without moving the authenticated seal and failing its checker.
USER root
RUN /usr/bin/python3 -I /opt/ra8-uv-bootstrap/managed_python_env.py write \
      --env "${PYTHON_TOOL_VENV}" --pyproject /opt/ra8-python-project/pyproject.toml \
      --lock /opt/ra8-python-project/uv.lock --group ci \
    && /usr/bin/python3 -I /opt/ra8-uv-bootstrap/managed_python_env.py verify \
      --env "${PYTHON_TOOL_VENV}" --pyproject /opt/ra8-python-project/pyproject.toml \
      --lock /opt/ra8-python-project/uv.lock --group ci
USER ${USERNAME}
