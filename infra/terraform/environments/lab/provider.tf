provider "vault" {
  address = var.openbao_address
  # The dedicated AppRole is deliberately not allowed to create child tokens.
  # Its own token is short-lived and read-only on the single Terraform secret.
  skip_child_token = true

  # The AppRole is dedicated to Terraform and is read-only on exactly one KV
  # path. The wrapper supplies these values from protected local credentials.
  auth_login {
    path = var.openbao_auth_path

    parameters = {
      role_id   = var.openbao_role_id
      secret_id = var.openbao_secret_id
    }
  }
}

# Ephemeral values are available during the run but are not persisted in the
# Terraform state. Terraform still needs protected state and plan handling for
# any downstream provider that receives secret-derived configuration.
ephemeral "vault_kv_secret_v2" "proxmox_api" {
  count = var.lab_enabled ? 1 : 0
  mount = var.openbao_kv_mount
  name  = var.openbao_secret_path
}

provider "proxmox" {
  # The endpoint is intentionally supplied at runtime. No production endpoint
  # belongs in the repository.
  endpoint = var.lab_enabled ? var.proxmox_endpoint : "https://127.0.0.1:8006"
  # The disabled plan needs a syntactically present, deliberately invalid
  # credential because the provider validates its configuration eagerly.
  api_token = var.lab_enabled ? ephemeral.vault_kv_secret_v2.proxmox_api[0].data["api_token"] : "disabled@pve!none=00000000-0000-0000-0000-000000000000"
  insecure  = var.proxmox_insecure

  # Do not let the provider pick an ID that happens to collide with an
  # operator's work. Lab IDs are explicit inputs when the environment is
  # eventually enabled.
  random_vm_ids = false
}
