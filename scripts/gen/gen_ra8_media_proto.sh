#!/usr/bin/env bash
# Regenerate the checked-in RA8 media-download protobuf-c codec.

set -euo pipefail

ROOT=$(git rev-parse --show-toplevel)
PROTO="$ROOT/libs/ra8_c6link/proto/ra8_media_download.proto"
HEADER="$ROOT/libs/ra8_c6link/inc/ra8_media_download.pb-c.h"
SOURCE="$ROOT/libs/ra8_c6link/src/ra8_media_download.pb-c.c"
MODE=${1:---write}

if [[ "$MODE" != "--write" && "$MODE" != "--check" ]]; then
  echo "usage: $0 [--write|--check]" >&2
  exit 2
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

if [[ "$MODE" == "--check" ]]; then
  cmp "$TMP/ra8_media_download.pb-c.h" "$HEADER" || {
    echo "gen_ra8_media_proto: generated header is stale" >&2
    exit 1
  }
  cmp "$TMP/ra8_media_download.pb-c.c" "$SOURCE" || {
    echo "gen_ra8_media_proto: generated source is stale" >&2
    exit 1
  }
  echo "gen_ra8_media_proto: generated codec is current"
  exit 0
fi

install -m 0644 "$TMP/ra8_media_download.pb-c.h" "$HEADER"
install -m 0644 "$TMP/ra8_media_download.pb-c.c" "$SOURCE"
echo "gen_ra8_media_proto: regenerated codec with protobuf-c 1.5.2 / libprotoc 35.1"
