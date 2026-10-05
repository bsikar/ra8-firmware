#!/bin/bash -p
# Load the OpenTofu/OpenBao runtime context without putting secret values in
# the repository, command arguments, or shell history.

set -euo pipefail
umask 077

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
repo_root="$(cd -- "$script_dir/../.." && pwd -P)"
operator_home="${HOME:?HOME is required}"
bao_env_file="${RA8_OPENBAO_ENV:-$operator_home/.config/hil/openbao.env}"
terraform_environment="${RA8_TOFU_ENV:-lab}"
case "$terraform_environment" in
  lab | lab-guest | ra8ci-runner) ;;
  *)
    printf '%s\n' 'error: unsupported OpenTofu environment.' >&2
    exit 1
    ;;
esac
terraform_root="$repo_root/infra/terraform/environments/$terraform_environment"
[[ -d "$terraform_root" ]] || {
  printf '%s\n' 'error: selected OpenTofu environment is unavailable.' >&2
  exit 1
}

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
terraform_state_key="$(security find-generic-password \
  -s "ra8-firmware/opentofu/state-encryption/$terraform_environment" \
  -a 'terraform-proxmox' -w)"

if [[ -z "$terraform_role_id" || -z "$terraform_secret_id" || -z "$terraform_state_key" ]]; then
  printf '%s\n' 'error: protected OpenBao credentials or the lab state key are unavailable.' >&2
  exit 1
fi

export TF_VAR_openbao_address="$bao_address"
export TF_VAR_openbao_role_id="$terraform_role_id"
export TF_VAR_openbao_secret_id="$terraform_secret_id"
export TF_VAR_state_encryption_passphrase="$terraform_state_key"
unset TF_VAR_proxmox_api_token
unset terraform_role_id terraform_secret_id terraform_state_key

exec tofu -chdir="$terraform_root" "$@"
