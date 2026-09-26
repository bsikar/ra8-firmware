#!/bin/bash -p
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Build the reviewed Debian cloud-image template used by the disposable
# Proxmox Linux CI driver. The script talks to Proxmox only through `pve` and
# writes only VM 9001 and the dedicated ra8-tf-lab datastore.

set -euo pipefail
umask 077

SSH_ALIAS="pve"
TEMPLATE_ID=9001
TEMPLATE_NAME="ra8-lab-debian-template"
POOL_ID="ra8-tf-lab"
STORAGE_ID="ra8-tf-lab"
IMAGE_NAME="debian-12-genericcloud-amd64.qcow2"
IMAGE_URL="https://cloud.debian.org/images/cloud/bookworm/latest/${IMAGE_NAME}"
CHECKSUM_URL="https://cloud.debian.org/images/cloud/bookworm/latest/SHA512SUMS"
run_dir=""
created=0

die() {
  printf 'error: %s\n' "$*" >&2
  exit 1
}

require_cmd() {
  command -v "$1" >/dev/null 2>&1 || die "required command is unavailable: $1"
}

remote_root() {
  ssh -o BatchMode=yes -o RequestTTY=no "$SSH_ALIAS" sudo -n /bin/bash -s -- "$@"
}

cleanup_remote_on_error() {
  local rc=$?
  trap - EXIT
if ((created)); then
    remote_root "$TEMPLATE_ID" "$TEMPLATE_NAME" <<'REMOTE' || true
set +e
template_id="$1"
expected_name="$2"
config="$(qm config "$template_id" 2>/dev/null)"
name="$(awk -F': ' '$1 == "name" {print substr($0, index($0, ": ") + 2); exit}' <<<"$config")"
description="$(awk -F': ' '$1 == "description" {print substr($0, index($0, ": ") + 2); exit}' <<<"$config")"
if [[ "$name" == "$expected_name" && "$description" == *"RA8_LAB_TEMPLATE=linux-ci-v1"* ]]; then
  qm set "$template_id" --protection 0 >/dev/null 2>&1
  qm destroy "$template_id" --purge 1 >/dev/null 2>&1
fi
REMOTE
fi
ssh -o BatchMode=yes -o RequestTTY=no "$SSH_ALIAS" sudo -n rm -f -- "/tmp/$IMAGE_NAME" >/dev/null 2>&1 || true
[[ -z "$run_dir" ]] || rm -rf -- "$run_dir"
  exit "$rc"
}

main() {
  for tool in curl sha512sum ssh scp; do
    require_cmd "$tool"
  done

  ssh -o BatchMode=yes -o RequestTTY=no "$SSH_ALIAS" /bin/true >/dev/null
  run_dir="$(mktemp -d "${TMPDIR:-/tmp}/ra8-lab-template.XXXXXXXX")"
  chmod 0700 "$run_dir"
  trap cleanup_remote_on_error EXIT

  curl -fsSL "$CHECKSUM_URL" -o "$run_dir/SHA512SUMS"
  expected_sha="$(awk -v image="$IMAGE_NAME" '$2 == image {print $1; exit}' "$run_dir/SHA512SUMS")"
  [[ "$expected_sha" =~ ^[0-9a-f]{128}$ ]] || die "official Debian checksum did not contain $IMAGE_NAME"
  curl -fsSL "$IMAGE_URL" -o "$run_dir/$IMAGE_NAME"
  printf '%s  %s\n' "$expected_sha" "$run_dir/$IMAGE_NAME" | sha512sum -c -
  scp -q "$run_dir/$IMAGE_NAME" "$SSH_ALIAS:/tmp/$IMAGE_NAME"
  created=1

  remote_root "$TEMPLATE_ID" "$TEMPLATE_NAME" "$POOL_ID" "$STORAGE_ID" "$IMAGE_NAME" <<'REMOTE'
set -euo pipefail
template_id="$1"
template_name="$2"
pool_id="$3"
storage_id="$4"
image_name="$5"
image_path="/tmp/$image_name"

[[ ! -e "/etc/pve/qemu-server/${template_id}.conf" ]] || {
  printf 'refusing to replace existing VM/template %s\n' "$template_id" >&2
  exit 1
}
[[ "$(pvesh get /pools/"$pool_id" --output-format json | jq -r '.poolid')" == "$pool_id" ]] || {
  printf 'required lab pool is unavailable\n' >&2
  exit 1
}
[[ "$(pvesm status | awk -v storage="$storage_id" '$1 == storage {print $3; exit}')" == "active" ]] || {
  printf 'required lab datastore is not active\n' >&2
  exit 1
}
[[ -s "$image_path" ]] || {
  printf 'uploaded Debian image is missing\n' >&2
  exit 1
}

qm create "$template_id" \
  --name "$template_name" \
  --memory 8192 \
  --cores 4 \
  --ostype l26 \
  --scsihw virtio-scsi-single \
  --pool "$pool_id" \
  --description "Disposable Debian 12 amd64 cloud template; RA8_LAB_TEMPLATE=linux-ci-v1" \
  --agent enabled=0 \
  --onboot 0
import_output="$(qm importdisk "$template_id" "$image_path" "$storage_id")"
unused_volume="$(qm config "$template_id" | awk -F': ' '$1 == "unused0" {print $2; exit}')"
[[ "$unused_volume" == "$storage_id:"* ]] || {
  printf 'imported image did not land on the dedicated lab datastore\n' >&2
  printf '%s\n' "$import_output" >&2
  exit 1
}
qm set "$template_id" --scsi0 "$unused_volume",discard=on,iothread=1 >/dev/null
qm resize "$template_id" scsi0 32G >/dev/null
qm set "$template_id" \
  --boot order=scsi0 \
  --serial0 socket \
  --vga serial0 \
  --description "Disposable Debian 12 amd64 cloud template; RA8_LAB_TEMPLATE=linux-ci-v1" >/dev/null
qm template "$template_id"
qm set "$template_id" --protection 1 >/dev/null
rm -f -- "$image_path"
REMOTE
  trap - EXIT
  rm -rf -- "$run_dir"
  printf '%s\n' 'created protected Debian 12 amd64 lab template 9001 on ra8-tf-lab'
}

main "$@"
