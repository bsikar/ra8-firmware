#!/bin/bash -p
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Wait for unattended Windows setup to shut down VM 9011 and seal as template.

set -euo pipefail

SSH_ALIAS="pve"
TEMPLATE_ID=9011
TEMPLATE_NAME="ra8-lab-windows-template"
STORAGE_ID="ra8-tf-lab"
UNATTEND_ISO_NAME="unattend-9011.iso"

remote_root() {
  ssh -o BatchMode=yes -o RequestTTY=no "$SSH_ALIAS" sudo -n /bin/bash -s -- "$@"
}

printf 'Monitoring VM %s until automated Windows setup shuts down the VM...\n' "$TEMPLATE_ID"
finished=0
for i in {1..120}; do
  status="$(ssh -o BatchMode=yes -o RequestTTY=no "$SSH_ALIAS" sudo -n qm status "$TEMPLATE_ID" 2>/dev/null | awk '{print $2}')"
  if [[ "$status" == "stopped" ]]; then
    finished=1
    break
  fi
  printf 'Poll %d: VM %s status is %s; sleeping 15s...\n' "$i" "$TEMPLATE_ID" "$status"
  sleep 15
done

((finished)) || {
  printf 'error: VM %s did not shut down within the expected time.\n' "$TEMPLATE_ID" >&2
  exit 1
}

printf 'Windows setup finished. Sealing VM %s as template...\n' "$TEMPLATE_ID"
remote_root "$TEMPLATE_ID" "$UNATTEND_ISO_NAME" "$STORAGE_ID" <<'REMOTE'
set -euo pipefail
template_id="$1"
unattend_iso_name="$2"
storage_id="$3"

# Detach installer ISOs
qm set "$template_id" --delete ide0,ide1,ide3 >/dev/null

# Attach Cloud-Init metadata drive on ide2
qm set "$template_id" --ide2 "${storage_id}:cloudinit" >/dev/null
qm set "$template_id" --boot order=sata0 >/dev/null

# Remove temporary unattend ISO from storage
rm -f "/var/lib/vz/template/iso/${unattend_iso_name}"

# Convert to template and protect
qm template "$template_id"
qm set "$template_id" --protection 1 >/dev/null
REMOTE

printf 'Successfully sealed VM %s (%s) as a protected template on %s.\n' "$TEMPLATE_ID" "$TEMPLATE_NAME" "$STORAGE_ID"
