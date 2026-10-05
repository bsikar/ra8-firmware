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

# The Go lifecycle observer needs the same pool-scoped API token that the
# OpenTofu provider reads ephemerally. The Darwin integration test asks this
# wrapper to place that token in a new owner-only file inside its private
# temporary directory; the value is never written to stdout or argv.
if [[ "${1:-}" == "--write-proxmox-token" ]]; then
  [[ $# -eq 2 && "$terraform_environment" == "ra8ci-runner" ]] || {
    printf '%s\n' 'error: token-file mode is limited to the ra8ci-runner environment.' >&2
    exit 1
  }
  token_file="$2"
  [[ "$token_file" == /* && "$token_file" != *$'\n'* ]] || {
    printf '%s\n' 'error: token destination must be an absolute file path.' >&2
    exit 1
  }
  token_directory="${token_file%/*}"
  [[ -n "$token_directory" && -d "$token_directory" && ! -L "$token_directory" ]] || {
    printf '%s\n' 'error: token destination directory must already exist.' >&2
    exit 1
  }
  directory_identity="$(stat -f '%u %Lp' "$token_directory" 2>/dev/null || true)"
  [[ "$directory_identity" == "$(id -u) 700" && ! -e "$token_file" && ! -L "$token_file" ]] || {
    printf '%s\n' 'error: token destination must be new in an owner-only directory.' >&2
    exit 1
  }
  command -v curl >/dev/null && command -v jq >/dev/null || {
    printf '%s\n' 'error: curl and jq are required for protected token retrieval.' >&2
    exit 1
  }

  terraform_role_id="$(security find-generic-password \
    -s 'ra8-firmware/openbao/terraform-proxmox-role' \
    -a 'terraform-proxmox' -w)"
  terraform_secret_id="$(security find-generic-password \
    -s 'ra8-firmware/openbao/terraform-proxmox-secret' \
    -a 'terraform-proxmox' -w)"
  [[ -n "$terraform_role_id" && -n "$terraform_secret_id" ]] || {
    printf '%s\n' 'error: protected OpenBao credentials are unavailable.' >&2
    exit 1
  }

  bao_api_root="${bao_address%/}/v1"
  # Match curl's transport to the configured OpenBao scheme, the same scheme
  # the OpenTofu vault provider already uses. An https address may name a
  # private CA through BAO_CACERT in the OpenBao env file or the environment.
  case "$bao_address" in
    https://*)
      curl_transport=(--proto '=https' --tlsv1.2)
      bao_cacert=""
      if [[ -r "$bao_env_file" ]]; then
        bao_cacert="$(awk -F= '$1 == "BAO_CACERT" {sub(/^[^=]*=/, ""); print; exit}' "$bao_env_file")"
      fi
      bao_cacert="${bao_cacert:-${BAO_CACERT:-}}"
      if [[ -n "$bao_cacert" ]]; then
        [[ -f "$bao_cacert" && -r "$bao_cacert" ]] || {
          printf '%s\n' 'error: BAO_CACERT does not name a readable file.' >&2
          exit 1
        }
        curl_transport+=(--cacert "$bao_cacert")
      fi
      ;;
    http://*)
      curl_transport=(--proto '=http')
      ;;
  esac
  login_payload="$(printf '%s\n%s' "$terraform_role_id" "$terraform_secret_id" |
    jq -R -s 'split("\n") | {role_id: .[0], secret_id: .[1]}')"
  login_response="$(printf '%s' "$login_payload" | curl --silent --show-error --fail \
    "${curl_transport[@]}" --header 'Content-Type: application/json' \
    --data-binary @- "$bao_api_root/auth/approle/login" 2>/dev/null)" || {
    printf 'error: OpenBao AppRole login failed (curl exit %s).\n' "$?" >&2
    exit 1
  }
  bao_token="$(printf '%s' "$login_response" | jq -er \
    '.auth.client_token | select(type == "string" and test("^[A-Za-z0-9._-]{16,1000}$"))')" || {
    printf '%s\n' 'error: OpenBao returned an invalid AppRole token.' >&2
    exit 1
  }
  temporary_token_file=""
  revoke_openbao_token() {
    [[ -n "${bao_token:-}" ]] || return 0
    printf 'header = "X-Vault-Token: %s"\n' "$bao_token" |
      curl --config - --silent --show-error --fail "${curl_transport[@]}" \
        --request POST --data '{}' "$bao_api_root/auth/token/revoke-self" >/dev/null 2>&1 || true
    bao_token=""
  }
  cleanup_token_export() {
    [[ -z "${temporary_token_file:-}" || ! -e "$temporary_token_file" ]] ||
      rm -f -- "$temporary_token_file"
    revoke_openbao_token
  }
  trap cleanup_token_export EXIT
  secret_response="$(printf 'header = "X-Vault-Token: %s"\n' "$bao_token" |
    curl --config - --silent --show-error --fail "${curl_transport[@]}" \
      "$bao_api_root/secret/data/terraform/proxmox-lab" 2>/dev/null)" || {
    printf 'error: OpenBao Proxmox token read failed (curl exit %s).\n' "$?" >&2
    exit 1
  }
  proxmox_api_token="$(printf '%s' "$secret_response" | jq -er \
    '.data.data.api_token | select(type == "string" and test("^[A-Za-z0-9_.-]+@[A-Za-z0-9_.-]+![A-Za-z0-9_.-]+=[A-Za-z0-9_-]+$"))')" || {
    printf '%s\n' 'error: OpenBao returned an invalid Proxmox API token.' >&2
    exit 1
  }
  temporary_token_file="$(mktemp "$token_directory/.proxmox-api-token.XXXXXX")" || {
    printf '%s\n' 'error: could not create a private token file.' >&2
    exit 1
  }
  chmod 600 "$temporary_token_file"
  printf '%s' "$proxmox_api_token" > "$temporary_token_file"
  mv -n "$temporary_token_file" "$token_file"
  [[ ! -e "$temporary_token_file" ]] || {
    rm -f -- "$temporary_token_file"
    printf '%s\n' 'error: token destination was created concurrently.' >&2
    exit 1
  }
  [[ -f "$token_file" && ! -L "$token_file" &&
    "$(stat -f '%u %Lp' "$token_file")" == "$(id -u) 600" ]] || {
    rm -f -- "$token_file"
    printf '%s\n' 'error: token file did not meet the private-file policy.' >&2
    exit 1
  }

  if ! printf 'header = "X-Vault-Token: %s"\n' "$bao_token" |
    curl --config - --silent --show-error --fail "${curl_transport[@]}" \
      --request POST --data '{}' "$bao_api_root/auth/token/revoke-self" >/dev/null 2>&1; then
    rm -f -- "$token_file"
    printf '%s\n' 'error: OpenBao token revocation failed.' >&2
    exit 1
  fi
  bao_token=""
  trap - EXIT
  unset terraform_role_id terraform_secret_id login_payload login_response bao_token secret_response proxmox_api_token
  printf '%s\n' 'Proxmox token written to an owner-only temporary file.'
  exit 0
fi

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
