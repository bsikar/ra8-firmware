#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# scripts/ci/devcontainer_image.sh -- THE definition of "the local ra8-ci image
# is the one this checkout describes", and the only thing that builds it.
#
# ===========================================================================
# WHY THIS FILE EXISTS
# ===========================================================================
# `make ci` boots a locally-built image tagged ra8-ci:latest. scripts/ci.sh
# used to build it once, when `image inspect` reported it absent, and reuse it
# from then on forever -- so a change to .devcontainer/Dockerfile refreshed the
# image on machines that had never built one, and on no other machine at all.
#
# The shared verification box was in exactly that state: the image it booted
# was built on 2026-07-20 and lacked cmake-format, cmake-lint, yamllint,
# actionlint, hadolint, gcc-14 and g++-14, so toolchain-parity, lint-cmake,
# lint-yaml and lint-devcontainer FAILED inside the container and PASSED
# natively on the same box, on the same commit (#521). That is the most
# expensive shape a failure can take: `make ci` is what CLAUDE.md tells every
# agent to run before a push, and four reds that have nothing to do with the
# change under test are indistinguishable from real ones until each is re-run
# by hand. The honest response costs time; the dishonest one ("that gate is
# always red here") is how a real regression gets waved through.
#
# `REBUILD=1 make ci` refreshed it, but nothing made that happen and nothing
# noticed that it had not. A build input nobody owns is not a build input.
#
# ===========================================================================
# HOW IT CANNOT GO STALE AGAIN
# ===========================================================================
# The image is a pure function of the .devcontainer build context, so the image
# RECORDS which context built it, in an OCI label, and a cached image whose
# label disagrees with the working tree is not reused -- it is rebuilt, loudly,
# saying which digest it carried and which the tree wants.
#
# Staleness is therefore detected from the tree the gates are about to run
# against, on every `make ci`, on every machine. It needs no timer, no converge
# and no human, and it works identically on the Mac -- which no Ansible run
# will ever reach -- as on the fleet.
#
# It is deliberately NOT an mtime comparison. Every file in a fresh clone or a
# `make ws-new` worktree carries today's mtime, so "the image is older than the
# Dockerfile" is true in a brand-new workspace whose image is perfectly
# current. A staleness check that cries wolf on an ordinary day teaches people
# to ignore it, which is the defect this file is written against, one level up.
#
# The dangerous direction is closed by construction rather than by care: an
# absent, misspelt or unreadable label yields an EMPTY digest, which never
# equals a sha256, so a broken read presents as a rebuild -- visible in
# seconds -- and can never present as a false "current".
#
# One consequence worth knowing before it surprises someone: there is a single
# tag, so two agents on one box whose branches carry DIFFERENT .devcontainer
# contexts will each rebuild it out from under the other. That is correct --
# every run gets the image its own tree describes -- and it is rare, because
# only a change to .devcontainer/ can cause it. Tagging per digest instead
# would avoid the churn at the price of an unbounded pile of images and a
# reaper to own it, which is a worse trade on a box that has already lost an
# image to a garbage collector (#484).
#
# ===========================================================================
# WHAT THE DIGEST COVERS
# ===========================================================================
# Every file under .devcontainer/, by path and by content: the Dockerfile, and
# also zshrc and p10k.zsh, which it COPYs. (scripts/ci.sh carried a comment
# claiming the Dockerfile has no COPY/ADD; it has had two since the shell
# configuration moved in, and digesting the Dockerfile alone would have missed
# a change to either.) File MODES are not covered -- nothing in this context is
# executable, and the Dockerfile chowns what it copies -- so a chmod-only
# change will not force a rebuild. Everything else about the context will.
#
# ===========================================================================
# USAGE
# ===========================================================================
#   devcontainer_image.sh digest              the working tree's context digest
#   devcontainer_image.sh state               current | stale | absent
#   devcontainer_image.sh ensure              build unless the cache is current
#   devcontainer_image.sh ensure --rebuild    build regardless
#   devcontainer_image.sh --selftest          prove the digest reacts, and that
#                                             the label round-trips
#
# Environment:
#   RA8_CI_IMAGE            image tag to manage      (default ra8-ci:latest)
#   RA8_CONTAINER_RUNTIME   runtime, may carry args  (default: the first of
#                           podman / docker / nerdctl on PATH). scripts/ci.sh
#                           exports the runtime it already resolved, so the two
#                           can never pick different ones.
#   RA8_IMAGE_LOCK_DIR      where the build lock lives (default: the pinned-tool
#                           cache, /var/cache/ra8-tools)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
CONTEXT_DIR="$REPO_ROOT/.devcontainer"

IMAGE_TAG="${RA8_CI_IMAGE:-ra8-ci:latest}"

# The OCI label the digest is stored in. Namespaced so it cannot collide with a
# label the Ubuntu base image sets.
LABEL_KEY="org.ra8.devcontainer-context"

# Print a fatal message and exit non-zero. Provisioning problems must be loud:
# a silent fallback here is what put a 2026-07-20 image under a 2026-07-28 tree.
die() {
  echo "ERROR: $*" >&2
  exit 1
}

# The container runtime as one argv line per word, for `mapfile`. Mirrors
# scripts/ci.sh exactly, including the "sudo podman" case the verification box
# needs: rootless podman cannot BUILD the devcontainer inside an unprivileged
# LXC, because apt's setgroups(2) is denied in the nested user namespace.
runtime_cmd() {
  local -a cmd
  read -r -a cmd <<<"${RA8_CONTAINER_RUNTIME:-}"
  if [[ "${#cmd[@]}" -eq 0 ]]; then
    local candidate
    for candidate in podman docker nerdctl; do
      if command -v "$candidate" >/dev/null 2>&1; then
        cmd=("$candidate")
        break
      fi
    done
  fi
  [[ "${#cmd[@]}" -eq 0 ]] || printf '%s\n' "${cmd[@]}"
}

# Resolve the runtime into RUNTIME, or die naming what is missing.
require_runtime() {
  mapfile -t RUNTIME < <(runtime_cmd)
  [[ "${#RUNTIME[@]}" -gt 0 ]] ||
    die "no container runtime on PATH (looked for podman, docker, nerdctl)."
}

# sha256 of stdin, as bare hex. Linux ships sha256sum and macOS ships shasum;
# every machine that runs these gates has one of the two.
sha256_stdin() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum | cut -d' ' -f1
  elif command -v shasum >/dev/null 2>&1; then
    shasum -a 256 | cut -d' ' -f1
  else
    die "neither sha256sum nor shasum is on PATH; the build context cannot be digested."
  fi
}

# Digest of a build context: one hash over a sorted manifest of
# "<content hash>  <path>" for every file in it. Sorting is what makes it
# reproducible -- readdir order is not.
#
# Args: $1 context directory (default: this checkout's .devcontainer)
context_digest() {
  local dir="${1:-$CONTEXT_DIR}"
  [[ -f "$dir/Dockerfile" ]] || die "no build context at $dir (expected $dir/Dockerfile)."
  local manifest
  manifest="$(
    cd "$dir" || exit 1
    find . -type f -print0 | LC_ALL=C sort -z | while IFS= read -r -d '' file; do
      printf '%s  %s\n' "$(sha256_stdin <"$file")" "$file"
    done
  )"
  [[ -n "$manifest" ]] || die "the build context at $dir holds no files."
  printf '%s\n' "$manifest" | sha256_stdin
}

# The digest recorded in an image, or nothing when the image is absent or
# predates this labelling. Both label locations are consulted: docker reports
# .Config.Labels and podman reports both. An image built before #521 reports
# neither, which is a stale image rather than an error.
#
# Args: $1 image reference (default: the managed tag)
image_digest() {
  local image="${1:-$IMAGE_TAG}"
  require_runtime
  "${RUNTIME[@]}" image inspect "$image" >/dev/null 2>&1 || return 0
  local value template
  for template in '{{ index .Config.Labels "'"$LABEL_KEY"'" }}' \
    '{{ index .Labels "'"$LABEL_KEY"'" }}'; do
    value="$("${RUNTIME[@]}" image inspect --format "$template" "$image" 2>/dev/null || true)"
    # A Go template prints "<no value>" for a missing key on some runtimes and
    # an empty string on others. Neither is a digest, and only a digest counts.
    if [[ "$value" =~ ^[0-9a-f]{64}$ ]]; then
      printf '%s\n' "$value"
      return 0
    fi
  done
}

# current | stale | absent, for the managed image against the working tree.
#
# The tree's digest is taken into a variable FIRST, deliberately. Compared
# inline inside [[ ]], a context_digest that died -- no checkout, no
# .devcontainer -- would contribute an empty string that an unlabelled image's
# equally empty digest would MATCH, reporting "current" for a comparison that
# never happened. As an assignment, `set -e` takes the run down instead, which
# is the only answer a broken comparison is allowed to give.
image_state() {
  require_runtime
  local want have
  want="$(context_digest)"
  if ! "${RUNTIME[@]}" image inspect "$IMAGE_TAG" >/dev/null 2>&1; then
    printf 'absent\n'
    return 0
  fi
  have="$(image_digest)"
  if [[ -n "$want" && "$have" == "$want" ]]; then
    printf 'current\n'
  else
    printf 'stale\n'
  fi
}

# Build the image and stamp the context digest onto it. The label is applied by
# the build that produced it, so the image and its digest cannot be set apart.
build_image() {
  local want="$1"
  require_runtime
  echo "==> building $IMAGE_TAG from .devcontainer/Dockerfile (runtime: ${RUNTIME[*]})"
  echo "    context digest $want"
  "${RUNTIME[@]}" build \
    --label "$LABEL_KEY=$want" \
    -t "$IMAGE_TAG" \
    -f "$CONTEXT_DIR/Dockerfile" \
    "$CONTEXT_DIR"
}

# Say why a build is about to happen, in terms the operator can act on.
announce() {
  local state="$1" want="$2" have="$3"
  if [[ "$state" == "absent" ]]; then
    echo "==> $IMAGE_TAG is not present on this machine; building it."
    return
  fi
  echo "==> cached $IMAGE_TAG was built from a DIFFERENT .devcontainer context."
  echo "    image: ${have:-(no context label -- built before #521 recorded one)}"
  echo "    tree:  $want"
  echo "    Rebuilding. A cached image that predates the Dockerfile is how four"
  echo "    gates came to fail in the container and pass natively on one box (#521)."
}

# Build under an exclusive lock where one is available.
#
# Not tuning. The verification box is shared: several agents run `make ci` at
# once, and the first run after a Dockerfile change would otherwise start one
# full apt-heavy build PER agent, simultaneously. Load on that box has already
# been measured turning gates red on its own, so a stampede here would
# manufacture exactly the false failures this file exists to remove. Waiters
# re-check under the lock, so all but one find a current image and build
# nothing.
#
# flock(1) is Linux-only (macOS ships no equivalent), so the Mac path builds
# unlocked -- which is correct there, where one developer runs one suite.
build_locked() {
  local want="$1" state="$2" have="$3"
  local lock_dir="${RA8_IMAGE_LOCK_DIR:-${RA8_TOOLS_CACHE_DIR:-/var/cache/ra8-tools}}"
  mkdir -p "$lock_dir" 2>/dev/null || true
  if ! command -v flock >/dev/null 2>&1 || [[ ! -d "$lock_dir" || ! -w "$lock_dir" ]]; then
    announce "$state" "$want" "$have"
    build_image "$want"
    return
  fi
  (
    if ! flock -n 9; then
      echo "==> another run on this machine is building $IMAGE_TAG; waiting for it."
      flock 9
    fi
    if [[ "$(image_state)" == "current" ]]; then
      echo "==> $IMAGE_TAG was rebuilt by that run and is current; not building it again."
      exit 0
    fi
    announce "$state" "$want" "$(image_digest)"
    build_image "$want"
  ) 9>"$lock_dir/devcontainer-image.lock"
}

cmd_ensure() {
  local force="${1:-}"
  local want state have
  want="$(context_digest)"
  if [[ "$force" == "--rebuild" ]]; then
    echo "==> --rebuild / REBUILD=1: rebuilding $IMAGE_TAG unconditionally."
    build_image "$want"
    return
  fi
  state="$(image_state)"
  if [[ "$state" == "current" ]]; then
    echo "==> reusing cached $IMAGE_TAG (context digest $want; --rebuild to refresh)"
    return
  fi
  have="$(image_digest)"
  build_locked "$want" "$state" "$have"
}

# Build a throwaway one-instruction image carrying `$2` as the context label
# (or no label at all when `$2` is empty), and print what image_digest reads
# back out of it.
selftest_round_trip() {
  local tag="$1" label="$2" tmp
  tmp="$(mktemp -d)"
  printf 'FROM scratch\nLABEL org.ra8.selftest="1"\n' >"$tmp/Dockerfile"
  local -a label_arg=()
  [[ -n "$label" ]] && label_arg=(--label "$LABEL_KEY=$label")
  "${RUNTIME[@]}" build "${label_arg[@]}" -t "$tag" -f "$tmp/Dockerfile" "$tmp" >/dev/null
  rm -rf "$tmp"
  image_digest "$tag"
  "${RUNTIME[@]}" rmi -f "$tag" >/dev/null 2>&1 || true
}

# Prove the two properties everything else rests on, in both directions: a
# changed context must change the digest, and a build's label must come back
# out of the image it was applied to. The digest half needs no runtime and runs
# anywhere; the round-trip half needs one and says so rather than skipping
# quietly.
cmd_selftest() {
  local tmp base changed
  tmp="$(mktemp -d)"
  # A synthetic context, never this checkout's: a selftest that writes into
  # .devcontainer/ would leave a probe file behind on its first failure.
  printf 'FROM scratch\n' >"$tmp/Dockerfile"
  printf 'shell config\n' >"$tmp/zshrc"
  base="$(context_digest "$tmp")"
  [[ "$base" =~ ^[0-9a-f]{64}$ ]] || die "selftest: the context digest is not a sha256: '$base'"

  printf 'shell config, edited\n' >"$tmp/zshrc"
  changed="$(context_digest "$tmp")"
  [[ "$changed" != "$base" ]] ||
    die "selftest: editing a COPYed context file did NOT change the digest."
  printf 'shell config\n' >"$tmp/zshrc"
  [[ "$(context_digest "$tmp")" == "$base" ]] ||
    die "selftest: restoring the context did not restore the digest."

  printf 'extra\n' >"$tmp/extra"
  [[ "$(context_digest "$tmp")" != "$base" ]] ||
    die "selftest: adding a context file did NOT change the digest."
  rm -rf "$tmp"
  echo "selftest: the context digest reacts to content and to membership OK"

  require_runtime
  local read_back
  read_back="$(selftest_round_trip "ra8-ci-selftest-labelled:$$" "$base")"
  [[ "$read_back" == "$base" ]] ||
    die "selftest: the label did not round-trip (wrote $base, read '${read_back:-<empty>}')."
  echo "selftest: a context digest round-trips through an image label OK"

  read_back="$(selftest_round_trip "ra8-ci-selftest-bare:$$" "")"
  [[ -z "$read_back" ]] ||
    die "selftest: an unlabelled image reported a digest ('$read_back'); it must report none."
  echo "selftest: an unlabelled image reads as stale, never as current OK"
  echo "selftest: all assertions held (both directions)."
}

usage() {
  cat <<'EOF'
scripts/ci/devcontainer_image.sh -- keep ra8-ci:latest matching .devcontainer/

  digest              print the working tree's build-context digest
  state               current | stale | absent
  ensure              build the image unless the cached one is current
  ensure --rebuild    build it regardless
  --selftest          prove the digest reacts and the label round-trips

Environment: RA8_CI_IMAGE, RA8_CONTAINER_RUNTIME, RA8_IMAGE_LOCK_DIR.
EOF
}

# Resolved by require_runtime; declared here so `set -u` cannot abort a reader
# before the first resolution.
RUNTIME=()

main() {
  case "${1:-}" in
    digest) context_digest ;;
    state) image_state ;;
    ensure) cmd_ensure "${2:-}" ;;
    --selftest) cmd_selftest ;;
    -h | --help) usage ;;
    "") usage ;;
    *) die "unknown command '$1'. Try --help." ;;
  esac
}

main "$@"
