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
  mount = var.openbao_kv_mount
  name  = var.openbao_secret_path
}

provider "proxmox" {
  # The endpoint is intentionally supplied at runtime. No production endpoint
  # belongs in the repository.
  endpoint  = var.proxmox_endpoint
  api_token = ephemeral.vault_kv_secret_v2.proxmox_api.data["api_token"]
  insecure  = var.proxmox_insecure

  # Do not let the provider pick an ID that happens to collide with an
  # operator's work. Lab IDs are explicit inputs when the environment is
  # eventually enabled.
  random_vm_ids = false
}
