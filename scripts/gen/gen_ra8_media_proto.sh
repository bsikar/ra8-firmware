#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# Regenerate the checked-in RA8 media-download protobuf-c codec and the pairing
# manifest the artefact-freshness gate checks without the pinned generator (#715).
#
# Modes:
#   --write     regenerate the codec, then rewrite the pairing manifest (default)
#   --check     byte-exact freshness check; needs protobuf-c 1.5.2 / libprotoc 35.1
#   --selftest  prove --check both fires and stays quiet, with NO generator present
#
# --selftest exists because --check cannot run anywhere this repository is built:
# the pinned generator pair is absent from the dev box and from the CI image, so
# every comparison below is code that has never once executed. The selftest builds
# a throwaway git repo, puts a STUB protoc-c on PATH, and runs THIS script inside
# it in both directions. What that proves is the plumbing -- the post-processing,
# the byte-exact compare, the version pin, the missing-generator exit, and the
# pairing-manifest call. What it cannot prove is that the committed C is what the
# real protoc-c emits for this schema; only a regenerate with the pinned pair
# proves that, and #715 stays open for wiring that pair into the image.

set -euo pipefail

ROOT=$(git rev-parse --show-toplevel)
PROTO="$ROOT/libs/ra8_c6link/proto/ra8_media_download.proto"
HEADER="$ROOT/libs/ra8_c6link/inc/ra8_media_download.pb-c.h"
SOURCE="$ROOT/libs/ra8_c6link/src/ra8_media_download.pb-c.c"
SELF="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/$(basename "${BASH_SOURCE[0]}")"
# ---------------------------------------------------------------------------
# SELFTEST -- both directions, with no generator installed.
#
# Every case runs THIS script inside a throwaway git repository whose layout
# mirrors the real one, against a stub protoc-c. Nothing in the real tree is
# read or written, and the cases assert exit STATUS, which is what the gate
# reacts to, rather than message text.
# ---------------------------------------------------------------------------

#: The version text the stub reports when it is standing in for the pinned pair.
PINNED_VERSION=$'protobuf-c 1.5.2\nlibprotoc 35.1'

# Write a stub protoc-c into directory $1. It reports $STUB_PROTOC_VERSION for
# --version and derives its output from the digest of the input schema, so a
# schema edit without a regenerate changes what it emits, exactly as the real
# generator would.
write_stub_generator() {
  local bin=$1
  mkdir -p "$bin"
  cat >"$bin/protoc-c" <<'STUB'
#!/usr/bin/env bash
set -euo pipefail
if [[ "${1:-}" == "--version" ]]; then
  printf '%s\n' "$STUB_PROTOC_VERSION"
  exit 0
fi
proto=${*: -1}
stem=${proto%.proto}
sum=$(sha256sum "$proto" | cut -d' ' -f1)
{
  printf '/* stub codec generated from %s */\n' "$sum"
  printf 'typedef enum StubMessage {\n'
  printf '  STUB_MESSAGE_ZERO = 0\n'
  printf '} StubMessage;\n'
} >"$stem.pb-c.h"
{
  printf '/* stub codec generated from %s */\n' "$sum"
  printf 'const int stub_codec_marker = 1;\n'
} >"$stem.pb-c.c"
STUB
  chmod +x "$bin/protoc-c"
}

# Lay out a throwaway repository at $1: the three tracked paths' directories,
# a schema, the pairing checker this script shells out to, and a git root so
# `git rev-parse --show-toplevel` resolves to the fixture rather than the tree.
seed_selftest_repo() {
  local root=$1
  mkdir -p "$root/libs/ra8_c6link/proto" "$root/libs/ra8_c6link/inc" \
    "$root/libs/ra8_c6link/src" "$root/.github" "$root/scripts/checks"
  cp "$ROOT/scripts/checks/check_proto_codec_pairing.py" "$root/scripts/checks/"
  printf 'syntax = "proto3";\nmessage Chunk { uint32 index = 1; }\n' \
    >"$root/libs/ra8_c6link/proto/ra8_media_download.proto"
  git -C "$root" init -q
}

# Run this script inside fixture repo $1 with the remaining arguments.
run_in_fixture() {
  local root=$1
  shift
  (cd "$root" && bash "$SELF" "$@")
}

# Assert that command $3.. exits with status $2, naming case $1 either way.
expect_rc() {
  local desc=$1 want=$2
  shift 2
  local got=0
  "$@" >/dev/null 2>&1 || got=$?
  if [[ "$got" != "$want" ]]; then
    printf 'gen_ra8_media_proto: FAIL -- %s (expected exit %s, got %s)\n' \
      "$desc" "$want" "$got" >&2
    return 1
  fi
  printf '  ok  %s (exit %s)\n' "$desc" "$got"
}

selftest() (
  set -e
  local tmp
  tmp=$(mktemp -d)
  trap 'rm -rf "$tmp"' EXIT
  local repo="$tmp/repo" bin="$tmp/bin" clean_path="$PATH"
  seed_selftest_repo "$repo"
  write_stub_generator "$bin"
  export PATH="$bin:$clean_path"
  export STUB_PROTOC_VERSION="$PINNED_VERSION"

  local header="$repo/libs/ra8_c6link/inc/ra8_media_download.pb-c.h"
  local source="$repo/libs/ra8_c6link/src/ra8_media_download.pb-c.c"
  local proto="$repo/libs/ra8_c6link/proto/ra8_media_download.proto"
  local manifest="$repo/.github/proto-codec-pairing.txt"

  echo "gen_ra8_media_proto selftest: 7 cases against a stub generator"

  expect_rc "--write lays down the codec and the manifest" 0 \
    run_in_fixture "$repo" --write
  expect_rc "a just-regenerated tree stays quiet" 0 \
    run_in_fixture "$repo" --check

  # Direction two: each drift shape this script exists to catch.
  printf '/* hand edit */\n' >>"$header"
  expect_rc "hand edit to the generated header fails" 1 \
    run_in_fixture "$repo" --check
  run_in_fixture "$repo" --write >/dev/null

  printf '/* hand edit */\n' >>"$source"
  expect_rc "hand edit to the generated source fails" 1 \
    run_in_fixture "$repo" --check
  run_in_fixture "$repo" --write >/dev/null

  printf 'message Extra { uint32 added = 1; }\n' >>"$proto"
  expect_rc "schema change without a regenerate fails" 1 \
    run_in_fixture "$repo" --check
  run_in_fixture "$repo" --write >/dev/null

  grep -v 'ra8_media_download.pb-c.c$' "$manifest" >"$manifest.trimmed"
  mv "$manifest.trimmed" "$manifest"
  expect_rc "a manifest missing a tracked row fails" 1 \
    run_in_fixture "$repo" --check
  run_in_fixture "$repo" --write >/dev/null

  # The pin and the absence, which are exit 2 rather than 1: a generator that is
  # the wrong version, or missing, must stop the run instead of skipping it.
  STUB_PROTOC_VERSION=$'protobuf-c 1.4.1\nlibprotoc 27.0' \
    expect_rc "an unpinned generator version fails" 2 \
    run_in_fixture "$repo" --check
  PATH="$clean_path" \
    expect_rc "an absent generator fails rather than skipping" 2 \
    run_in_fixture "$repo" --check

  echo "gen_ra8_media_proto: selftest passed"
)

MODE=${1:---write}

if [[ "$MODE" != "--write" && "$MODE" != "--check" && "$MODE" != "--selftest" ]]; then
  echo "usage: $0 [--write|--check|--selftest]" >&2
  exit 2
fi

# The selftest stands in for the generator rather than requiring it, so it is
# dispatched BEFORE the version pin below. Its definitions live at the foot of
# this file, after the pipeline they exercise.
if [[ "$MODE" == "--selftest" ]]; then
  selftest
  exit $?
fi

command -v protoc-c >/dev/null 2>&1 || {
  echo "gen_ra8_media_proto: protoc-c 1.5.2 is required" >&2
  exit 2
}

VERSION=$(protoc-c --version 2>/dev/null | tail -n 2)
grep -qx 'protobuf-c 1.5.2' <<<"$VERSION" || {
  echo "gen_ra8_media_proto: expected protobuf-c 1.5.2" >&2
  echo "$VERSION" >&2
  exit 2
}
grep -qx 'libprotoc 35.1' <<<"$VERSION" || {
  echo "gen_ra8_media_proto: expected libprotoc 35.1" >&2
  echo "$VERSION" >&2
  exit 2
}

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
cp "$PROTO" "$TMP/ra8_media_download.proto"
(cd "$TMP" && protoc-c --c_out=. ra8_media_download.proto)

mark_generated_enums() {
  local path=$1
  local marked="$path.c23"
  awk '
    /^typedef enum [A-Za-z0-9_]+ \{$/ {
      print $0 " /* C23HDR-OK: protoc-c 1.5.2 owns this enum ABI. */"
      next
    }
    { print }
  ' "$path" >"$marked"
  mv "$marked" "$path"
}

prepend_attribution() {
  local path=$1
  local file_name=$2
  local brief=$3
  local c23_exception=${4:-}
  local attributed="$path.attributed"
  {
    printf '/**\n'
    printf ' * @file %s\n' "$file_name"
    printf ' * @brief %s\n' "$brief"
    printf ' *\n'
    printf ' * @details\n'
    printf '%s\n' \
      " * Generated from \`ra8_media_download.proto\` by the pinned protobuf-C compiler."
    printf '%s\n' \
      " * Do not edit this file directly; use \`scripts/gen/gen_ra8_media_proto.sh\` so"
    printf ' * the checked-in codec and its attribution remain reproducible.\n'
    if [[ -n "$c23_exception" ]]; then
      printf ' * C23HDR-OK: %s\n' "$c23_exception"
    fi
    printf ' *\n'
    printf ' * @copyright Copyright (c) 2026 Brighton Sikarskie\n'
    printf ' * SPDX-License-Identifier: MIT\n'
    printf ' */\n\n'
    command cat "$path"
  } >"$attributed"
  mv "$attributed" "$path"
}

mark_generated_enums "$TMP/ra8_media_download.pb-c.h"
prepend_attribution \
  "$TMP/ra8_media_download.pb-c.h" \
  'ra8_media_download.pb-c.h' \
  'Generated protobuf-C declarations for the media-download protocol.' \
  'protoc-c 1.5.2 owns the classic guard and enum ABI.'
prepend_attribution \
  "$TMP/ra8_media_download.pb-c.c" \
  'ra8_media_download.pb-c.c' \
  'Generated protobuf-C implementation of the media-download protocol.'

if [[ "$MODE" == "--check" ]]; then
  cmp "$TMP/ra8_media_download.pb-c.h" "$HEADER" || {
    echo "gen_ra8_media_proto: generated header is stale" >&2
    exit 1
  }
  cmp "$TMP/ra8_media_download.pb-c.c" "$SOURCE" || {
    echo "gen_ra8_media_proto: generated source is stale" >&2
    exit 1
  }
  python3 "$ROOT/scripts/checks/check_proto_codec_pairing.py" || {
    echo "gen_ra8_media_proto: pairing manifest is stale; run --write" >&2
    exit 1
  }
  echo "gen_ra8_media_proto: generated codec is current"
  exit 0
fi

install -m 0644 "$TMP/ra8_media_download.pb-c.h" "$HEADER"
install -m 0644 "$TMP/ra8_media_download.pb-c.c" "$SOURCE"

# The pairing manifest is rewritten in the SAME command that regenerates, so a real
# regenerate can never leave the artefact-freshness gate red, and only a hand edit or a
# forgotten regenerate can (#715). The generator is absent from the dev box and the CI
# image, which is why the digest gate exists alongside this byte-exact one.
python3 "$ROOT/scripts/checks/check_proto_codec_pairing.py" --write

echo "gen_ra8_media_proto: regenerated codec with protobuf-c 1.5.2 / libprotoc 35.1"
