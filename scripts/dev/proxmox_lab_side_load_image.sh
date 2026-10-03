#!/bin/bash -p
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Build the lab's Linux/amd64 CI image on the controller and export a pinned
# archive for transfer over the existing SSH path.

set -euo pipefail
umask 077

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
repo_root="$(cd -- "$script_dir/../.." && pwd -P)"
archive="${1:?usage: proxmox_lab_side_load_image.sh ARCHIVE METADATA}"
metadata="${2:?usage: proxmox_lab_side_load_image.sh ARCHIVE METADATA}"
image="ra8-ci:latest"

export RA8_CI_PLATFORM=linux/amd64
"$repo_root/scripts/ci/devcontainer_image.sh" ensure

declare -a runtime=()
if [[ -n "${RA8_CONTAINER_RUNTIME:-}" ]]; then
  read -r -a runtime <<<"$RA8_CONTAINER_RUNTIME"
else
  for candidate in podman docker nerdctl; do
    if command -v "$candidate" >/dev/null 2>&1 && "$candidate" info >/dev/null 2>&1; then
      runtime=("$candidate")
      break
    fi
  done
fi
(( ${#runtime[@]} > 0 )) || {
  printf '%s\n' 'error: no usable Podman, Docker, or nerdctl runtime is available to export the lab image.' >&2
  exit 1
}

identity="$("${runtime[@]}" image inspect --format '{{.Id}}|{{.Os}}/{{.Architecture}}' "$image")"
image_id="${identity%%|*}"
platform="${identity#*|}"
[[ "$image_id" =~ ^sha256:[0-9a-f]{64}$ ]] || {
  printf '%s\n' 'error: container runtime returned an invalid image configuration digest.' >&2
  exit 1
}
[[ "$platform" == linux/amd64 ]] || {
  printf "error: lab image has platform %s; expected linux/amd64.\n" "$platform" >&2
  exit 1
}

mkdir -p "$(dirname -- "$archive")" "$(dirname -- "$metadata")"
"${runtime[@]}" save --output "$archive" "$image"
chmod 0600 "$archive"
printf 'lab_ci_image_name: "%s"\nlab_ci_image_id: "%s"\nlab_ci_image_platform: "%s"\n' \
  "$image" "$image_id" "$platform" >"$metadata"
chmod 0600 "$metadata"
printf 'Side-loaded %s (%s) as %s.\n' "$image" "$platform" "$image_id"
