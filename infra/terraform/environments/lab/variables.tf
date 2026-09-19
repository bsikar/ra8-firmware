variable "proxmox_endpoint" {
  description = "Lab-only Proxmox API endpoint, supplied through TF_VAR_proxmox_endpoint."
  type        = string
  nullable    = true
  default     = null
}

variable "proxmox_insecure" {
  description = "Whether to skip Proxmox API TLS verification; keep false for normal operation."
  type        = bool
  default     = false
}

variable "openbao_address" {
  description = "OpenBao API address, supplied at runtime through TF_VAR_openbao_address."
  type        = string
  nullable    = false

  validation {
    condition     = can(regex("^https?://[^[:space:]]+$", trimspace(var.openbao_address)))
    error_message = "openbao_address must be an HTTP(S) URL supplied through the protected runtime environment."
  }
}

variable "openbao_role_id" {
  description = "Dedicated Terraform OpenBao AppRole ID, supplied through TF_VAR_openbao_role_id."
  type        = string
  nullable    = false
  sensitive   = true
}

variable "openbao_secret_id" {
  description = "Dedicated Terraform OpenBao AppRole secret ID, supplied through TF_VAR_openbao_secret_id."
  type        = string
  nullable    = false
  sensitive   = true
}

variable "openbao_auth_path" {
  description = "OpenBao AppRole login path."
  type        = string
  default     = "auth/approle/login"
}

variable "openbao_kv_mount" {
  description = "OpenBao KV v2 mount containing the Proxmox token."
  type        = string
  default     = "secret"
}

variable "openbao_secret_path" {
  description = "OpenBao KV v2 path containing the Proxmox token."
  type        = string
  default     = "terraform/proxmox-lab"
}

variable "lab_enabled" {
  description = "Whether the disposable lab guests are declared. Keep false until reviewed."
  type        = bool
  default     = false
}

variable "allow_lxc" {
  description = "Explicit second-review gate for the lower-isolation LXC path."
  type        = bool
  default     = false
}

variable "lab_vm" {
  description = "Configuration for the optional disposable lab VM."
  type = object({
    enabled         = bool
    name            = string
    vm_id           = number
    template_vm_id  = number
    pool_id         = string
    datastore_id    = string
    node_name       = string
    cores           = number
    memory_mb       = number
    bridge          = string
    ipv4_address    = string
    ipv4_gateway    = string
    ssh_public_keys = list(string)
    user_name       = string
  })

  default = {
    enabled         = false
    name            = ""
    vm_id           = null
    template_vm_id  = null
    pool_id         = ""
    datastore_id    = ""
    node_name       = ""
    cores           = 2
    memory_mb       = 4096
    bridge          = ""
    ipv4_address    = "dhcp"
    ipv4_gateway    = null
    ssh_public_keys = []
    user_name       = "terraform-lab"
  }

  validation {
    condition = !var.lab_enabled || !var.lab_vm.enabled || (
      trimspace(var.lab_vm.name) != "" &&
      startswith(lower(trimspace(var.lab_vm.name)), "ra8-lab-") &&
      var.lab_vm.vm_id != null &&
      var.lab_vm.vm_id >= 9000 &&
      var.lab_vm.vm_id <= 9099 &&
      var.lab_vm.template_vm_id != null &&
      var.lab_vm.template_vm_id >= 9000 &&
      var.lab_vm.template_vm_id <= 9099 &&
      lower(trimspace(var.lab_vm.pool_id)) == "ra8-tf-lab" &&
      lower(trimspace(var.lab_vm.datastore_id)) == "ra8-tf-lab" &&
      trimspace(var.lab_vm.node_name) != "" &&
      lower(trimspace(var.lab_vm.bridge)) == "vmbr9" &&
      var.lab_vm.cores >= 1 &&
      var.lab_vm.cores <= 4 &&
      var.lab_vm.memory_mb >= 512 &&
      var.lab_vm.memory_mb <= 8192 &&
      lower(trimspace(var.lab_vm.ipv4_address)) == "dhcp" &&
      var.lab_vm.ipv4_gateway == null &&
      length(var.lab_vm.ssh_public_keys) > 0
    )
    error_message = "An enabled lab VM requires ra8-lab-* naming, VM/template IDs in 9000-9099, the exact ra8-tf-lab pool and datastore, the exact vmbr9 bridge, 1-4 cores, 512-8192 MB, DHCP without a gateway, and at least one SSH public key."
  }
}

variable "lab_container" {
  description = "Configuration for the optional disposable lab container."
  type = object({
    enabled         = bool
    hostname        = string
    vm_id           = number
    template_ct_id  = number
    pool_id         = string
    datastore_id    = string
    node_name       = string
    cores           = number
    memory_mb       = number
    bridge          = string
    ipv4_address    = string
    ipv4_gateway    = string
    ssh_public_keys = list(string)
  })

  default = {
    enabled         = false
    hostname        = ""
    vm_id           = null
    template_ct_id  = null
    pool_id         = ""
    datastore_id    = ""
    node_name       = ""
    cores           = 2
    memory_mb       = 2048
    bridge          = ""
    ipv4_address    = "dhcp"
    ipv4_gateway    = null
    ssh_public_keys = []
  }

  validation {
    condition     = !var.lab_container.enabled || var.allow_lxc
    error_message = "The LXC lab path requires an explicit allow_lxc=true review decision."
  }

  validation {
    condition = !var.lab_enabled || !var.lab_container.enabled || (
      trimspace(var.lab_container.hostname) != "" &&
      startswith(lower(trimspace(var.lab_container.hostname)), "ra8-lab-") &&
      var.lab_container.vm_id != null &&
      var.lab_container.vm_id >= 9000 &&
      var.lab_container.vm_id <= 9099 &&
      var.lab_container.template_ct_id != null &&
      var.lab_container.template_ct_id >= 9000 &&
      var.lab_container.template_ct_id <= 9099 &&
      lower(trimspace(var.lab_container.pool_id)) == "ra8-tf-lab" &&
      lower(trimspace(var.lab_container.datastore_id)) == "ra8-tf-lab" &&
      trimspace(var.lab_container.node_name) != "" &&
      lower(trimspace(var.lab_container.bridge)) == "vmbr9" &&
      var.lab_container.cores >= 1 &&
      var.lab_container.cores <= 2 &&
      var.lab_container.memory_mb >= 256 &&
      var.lab_container.memory_mb <= 4096 &&
      lower(trimspace(var.lab_container.ipv4_address)) == "dhcp" &&
      var.lab_container.ipv4_gateway == null &&
      length(var.lab_container.ssh_public_keys) > 0
    )
    error_message = "An enabled lab container requires ra8-lab-* naming, container/template IDs in 9000-9099, the exact ra8-tf-lab pool and datastore, the exact vmbr9 bridge, 1-2 cores, 256-4096 MB, DHCP without a gateway, and at least one SSH public key."
  }
}
