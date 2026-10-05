variable "state_encryption_passphrase" {
  description = "Per-environment OpenTofu state and plan key injected from protected operator storage."
  type        = string
  sensitive   = true
  nullable    = false
}

variable "runner_enabled" {
  description = "Explicit gate for declaring an ephemeral runner VM in this root."
  type        = bool
  default     = false
}

variable "proxmox_endpoint" {
  description = "Reviewed Proxmox API origin supplied from protected runtime configuration."
  type        = string
}

variable "openbao_address" {
  description = "OpenBao API address."
  type        = string
}

variable "openbao_kv_mount" {
  type    = string
  default = "secret"
}

variable "openbao_secret_path" {
  type    = string
  default = "terraform/proxmox-lab"
}

variable "runner" {
  description = "One exact server-reserved ephemeral Linux runner identity."
  type = object({
    reservation_id        = string
    creation_operation_id = string
    run_id                = string
    vm_id                 = number
    template_vm_id        = number
    node_name             = string
    pool_id               = string
    datastore_id          = string
    bridge                = string
    cores                 = number
    memory_mb             = number
    ipv4_address          = string
    ipv4_gateway          = string
    ssh_public_keys       = list(string)
    user_name             = string
    started               = bool
    network_enabled       = bool
  })

  default  = null
  nullable = true

  validation {
    condition     = !var.runner_enabled || var.runner != null
    error_message = "runner must be set when runner_enabled is true."
  }
}
