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

variable "lab_linux_vm" {
  description = "Configuration for the optional disposable Linux lab VM."
  type = object({
    enabled         = bool
    name            = optional(string, "")
    run_id          = optional(string, "")
    vm_id           = optional(number, null)
    template_vm_id  = optional(number, null)
    pool_id         = optional(string, "")
    datastore_id    = optional(string, "")
    node_name       = optional(string, "")
    cores           = optional(number, 2)
    memory_mb       = optional(number, 4096)
    bridge          = optional(string, "")
    ipv4_address    = optional(string, "dhcp")
    ipv4_gateway    = optional(string, null)
    ssh_public_keys = optional(list(string), [])
    user_name       = optional(string, "terraform-lab")
    started         = optional(bool, false)
    network_enabled = optional(bool, false)
  })

  default = {
    enabled         = false
    name            = ""
    run_id          = ""
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
    started         = false
    network_enabled = false
  }

  validation {
    condition = !var.lab_enabled || !var.lab_linux_vm.enabled || (
      trimspace(var.lab_linux_vm.name) != "" &&
      startswith(lower(trimspace(var.lab_linux_vm.name)), "ra8-lab-") &&
      can(regex("^[0-9a-f]{16}$", trimspace(var.lab_linux_vm.run_id))) &&
      var.lab_linux_vm.vm_id != null &&
      var.lab_linux_vm.vm_id >= 9000 &&
      var.lab_linux_vm.vm_id <= 9099 &&
      var.lab_linux_vm.template_vm_id != null &&
      var.lab_linux_vm.template_vm_id >= 9000 &&
      var.lab_linux_vm.template_vm_id <= 9099 &&
      lower(trimspace(var.lab_linux_vm.pool_id)) == "ra8-tf-lab" &&
      lower(trimspace(var.lab_linux_vm.datastore_id)) == "ra8-tf-lab" &&
      trimspace(var.lab_linux_vm.node_name) != "" &&
      (lower(trimspace(var.lab_linux_vm.bridge)) == "vmbr8" || lower(trimspace(var.lab_linux_vm.bridge)) == "vmbr9") &&
      var.lab_linux_vm.cores >= 1 &&
      var.lab_linux_vm.cores <= 4 &&
      var.lab_linux_vm.memory_mb >= 512 &&
      var.lab_linux_vm.memory_mb <= 8192 &&
      ((!var.lab_linux_vm.network_enabled && lower(trimspace(var.lab_linux_vm.ipv4_address)) == "dhcp" && var.lab_linux_vm.ipv4_gateway == null) ||
      (var.lab_linux_vm.network_enabled && can(regex("^10\\.250\\.[89]\\.[0-9]{1,3}/24$", trimspace(var.lab_linux_vm.ipv4_address))) && (var.lab_linux_vm.ipv4_gateway == "10.250.8.1" || var.lab_linux_vm.ipv4_gateway == "10.250.9.1"))) &&
      length(var.lab_linux_vm.ssh_public_keys) > 0 &&
      (!var.lab_linux_vm.network_enabled || var.lab_linux_vm.started)
    )
    error_message = "An enabled Linux lab VM requires ra8-lab-* naming, a 16-hex run ID, VM/template IDs in 9000-9099, the exact ra8-tf-lab pool and datastore, an approved lab bridge (vmbr8 or vmbr9), 1-4 cores, 512-8192 MB, either disconnected DHCP or the exact lab network, at least one SSH public key, and network_enabled requires started."
  }
}

variable "lab_vm" {
  description = "Deprecated alias for lab_linux_vm."
  type = object({
    enabled         = bool
    name            = optional(string, "")
    run_id          = optional(string, "")
    vm_id           = optional(number, null)
    template_vm_id  = optional(number, null)
    pool_id         = optional(string, "")
    datastore_id    = optional(string, "")
    node_name       = optional(string, "")
    cores           = optional(number, 2)
    memory_mb       = optional(number, 4096)
    bridge          = optional(string, "")
    ipv4_address    = optional(string, "dhcp")
    ipv4_gateway    = optional(string, null)
    ssh_public_keys = optional(list(string), [])
    user_name       = optional(string, "terraform-lab")
    started         = optional(bool, false)
    network_enabled = optional(bool, false)
  })
  default = null
}

variable "lab_windows_vm" {
  description = "Configuration for the optional disposable Windows Server amd64 lab VM."
  type = object({
    enabled         = bool
    name            = optional(string, "")
    run_id          = optional(string, "")
    vm_id           = optional(number, null)
    template_vm_id  = optional(number, null)
    pool_id         = optional(string, "")
    datastore_id    = optional(string, "")
    node_name       = optional(string, "")
    cores           = optional(number, 4)
    memory_mb       = optional(number, 8192)
    bridge          = optional(string, "")
    ipv4_address    = optional(string, "dhcp")
    ipv4_gateway    = optional(string, null)
    ssh_public_keys = optional(list(string), [])
    user_name       = optional(string, "Administrator")
    started         = optional(bool, false)
    network_enabled = optional(bool, false)
  })

  default = {
    enabled         = false
    name            = ""
    run_id          = ""
    vm_id           = null
    template_vm_id  = null
    pool_id         = ""
    datastore_id    = ""
    node_name       = ""
    cores           = 4
    memory_mb       = 8192
    bridge          = ""
    ipv4_address    = "dhcp"
    ipv4_gateway    = null
    ssh_public_keys = []
    user_name       = "Administrator"
    started         = false
    network_enabled = false
  }

  validation {
    condition = !var.lab_enabled || !var.lab_windows_vm.enabled || (
      trimspace(var.lab_windows_vm.name) != "" &&
      startswith(lower(trimspace(var.lab_windows_vm.name)), "ra8-lab-win-") &&
      can(regex("^[0-9a-f]{16}$", trimspace(var.lab_windows_vm.run_id))) &&
      var.lab_windows_vm.vm_id != null &&
      var.lab_windows_vm.vm_id >= 9000 &&
      var.lab_windows_vm.vm_id <= 9099 &&
      var.lab_windows_vm.template_vm_id != null &&
      var.lab_windows_vm.template_vm_id >= 9000 &&
      var.lab_windows_vm.template_vm_id <= 9099 &&
      lower(trimspace(var.lab_windows_vm.pool_id)) == "ra8-tf-lab" &&
      lower(trimspace(var.lab_windows_vm.datastore_id)) == "ra8-tf-lab" &&
      trimspace(var.lab_windows_vm.node_name) != "" &&
      (lower(trimspace(var.lab_windows_vm.bridge)) == "vmbr8" || lower(trimspace(var.lab_windows_vm.bridge)) == "vmbr9") &&
      var.lab_windows_vm.cores >= 2 &&
      var.lab_windows_vm.cores <= 4 &&
      var.lab_windows_vm.memory_mb >= 4096 &&
      var.lab_windows_vm.memory_mb <= 8192 &&
      ((!var.lab_windows_vm.network_enabled && lower(trimspace(var.lab_windows_vm.ipv4_address)) == "dhcp" && var.lab_windows_vm.ipv4_gateway == null) ||
      (var.lab_windows_vm.network_enabled && can(regex("^10\\.250\\.[89]\\.[0-9]{1,3}/24$", trimspace(var.lab_windows_vm.ipv4_address))) && (var.lab_windows_vm.ipv4_gateway == "10.250.8.1" || var.lab_windows_vm.ipv4_gateway == "10.250.9.1"))) &&
      (!var.lab_windows_vm.network_enabled || var.lab_windows_vm.started)
    )
    error_message = "An enabled Windows lab VM requires an ra8-lab-win-* name, a 16-hex run ID, VM/template IDs in 9000-9099, the exact ra8-tf-lab pool and datastore, an approved lab bridge (vmbr8 or vmbr9), 2-4 cores, 4096-8192 MB of memory, either disconnected DHCP or the exact lab network, network_enabled requires started, and a prebuilt Cloudbase-Init template."
  }
}

variable "lab_container" {
  description = "Configuration for the optional disposable lab container."
  type = object({
    enabled         = bool
    hostname        = optional(string, "")
    vm_id           = optional(number, null)
    template_ct_id  = optional(number, null)
    pool_id         = optional(string, "")
    datastore_id    = optional(string, "")
    node_name       = optional(string, "")
    cores           = optional(number, 2)
    memory_mb       = optional(number, 2048)
    bridge          = optional(string, "")
    ipv4_address    = optional(string, "dhcp")
    ipv4_gateway    = optional(string, null)
    ssh_public_keys = optional(list(string), [])
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
