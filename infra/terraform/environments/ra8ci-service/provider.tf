provider "vault" {
  address          = var.openbao_address
  skip_child_token = true

  auth_login {
    path = var.openbao_auth_path

    parameters = {
      role_id   = var.openbao_role_id
      secret_id = var.openbao_secret_id
    }
  }
}

# The dedicated AppRole may read only this one scoped Proxmox API token.
# Ephemeral secret values are not persisted in Terraform state.
ephemeral "vault_kv_secret_v2" "proxmox_api" {
  mount = var.openbao_kv_mount
  name  = var.openbao_secret_path
}

provider "proxmox" {
  endpoint      = var.proxmox_endpoint
  api_token     = ephemeral.vault_kv_secret_v2.proxmox_api.data["api_token"]
  insecure      = false
  random_vm_ids = false
}
