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

ephemeral "vault_kv_secret_v2" "proxmox_api" {
  count = var.runner_enabled ? 1 : 0
  mount = var.openbao_kv_mount
  name  = var.openbao_secret_path
}

provider "proxmox" {
  endpoint      = var.runner_enabled ? var.proxmox_endpoint : "https://127.0.0.1:8006"
  api_token     = var.runner_enabled ? ephemeral.vault_kv_secret_v2.proxmox_api[0].data["api_token"] : "disabled@pve!none=00000000-0000-0000-0000-000000000000"
  insecure      = false
  random_vm_ids = false
}
