#!/bin/bash -p
# Load the Terraform/OpenBao runtime context without putting secret values in
# the repository, command arguments, or shell history.

set -euo pipefail
umask 077

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
repo_root="$(cd -- "$script_dir/../.." && pwd -P)"
operator_home="${HOME:?HOME is required}"
bao_env_file="${RA8_OPENBAO_ENV:-$operator_home/.config/hil/openbao.env}"

if [[ -r "$bao_env_file" ]]; then
  bao_address="$(awk -F= '$1 == "BAO_ADDR" {sub(/^[^=]*=/, ""); print; exit}' "$bao_env_file")"
else
  bao_address="${BAO_ADDR:-}"
fi

case "$bao_address" in
  http://* | https://*) ;;
  *)
    printf '%s\n' 'error: OpenBao address is missing or is not HTTP(S).' >&2
    exit 1
    ;;
esac

terraform_role_id="$(security find-generic-password \
  -s 'ra8-firmware/openbao/terraform-proxmox-role' \
  -a 'terraform-proxmox' -w)"
terraform_secret_id="$(security find-generic-password \
  -s 'ra8-firmware/openbao/terraform-proxmox-secret' \
  -a 'terraform-proxmox' -w)"

if [[ -z "$terraform_role_id" || -z "$terraform_secret_id" ]]; then
  printf '%s\n' 'error: Terraform OpenBao AppRole credentials are unavailable.' >&2
  exit 1
fi

export TF_VAR_openbao_address="$bao_address"
export TF_VAR_openbao_role_id="$terraform_role_id"
export TF_VAR_openbao_secret_id="$terraform_secret_id"
unset TF_VAR_proxmox_api_token

exec terraform -chdir="$repo_root/infra/terraform/environments/lab" "$@"
