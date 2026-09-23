variable "service_enabled" {
  description = "Explicit gate for the protected, persistent ra8ci control VM."
  type        = bool
  default     = false
}

variable "network_approved" {
  description = "Operator attestation that the private management segment and firewall were reviewed."
  type        = bool
  default     = false
}

variable "off_vm_backup_approved" {
  description = "Operator attestation that an encrypted off-VM destination and restore procedure were approved."
  type        = bool
  default     = false
}

variable "off_vm_backup_target" {
  description = "Nonsecret identity of the approved off-VM backup destination for review."
  type        = string
  default     = ""
}

variable "proxmox_endpoint" {
  description = "Reviewed HTTPS Proxmox API endpoint, supplied only at runtime."
  type        = string
  default     = ""
  validation {
    condition     = !var.service_enabled || can(regex("^https://[^[:space:]]+:8006/?$", var.proxmox_endpoint))
    error_message = "An enabled service requires an explicit HTTPS Proxmox API endpoint on port 8006."
  }
}

variable "openbao_address" {
  description = "Reviewed HTTPS OpenBao endpoint, supplied only at runtime."
  type        = string
  default     = ""
  validation {
    condition     = !var.service_enabled || can(regex("^https://[^[:space:]]+$", var.openbao_address))
    error_message = "An enabled service requires an explicit HTTPS OpenBao address."
  }
}

variable "openbao_auth_path" {
  description = "Dedicated AppRole login path."
  type        = string
  default     = "auth/approle/login"
}

variable "openbao_role_id" {
  description = "Dedicated AppRole role ID from protected runtime input."
  type        = string
  sensitive   = true
  default     = ""
}

variable "openbao_secret_id" {
  description = "Dedicated AppRole secret ID from protected runtime input."
  type        = string
  sensitive   = true
  default     = ""
}

variable "openbao_kv_mount" {
  description = "KV v2 mount containing only the dedicated service Proxmox API token."
  type        = string
  default     = ""
}

variable "openbao_secret_path" {
  description = "KV v2 path containing only the dedicated service Proxmox API token."
  type        = string
  default     = ""
}

variable "vm_id" {
  description = "Unused persistent VM ID; 9000-9099 remain reserved for disposable lab guests."
  type        = number
  default     = null
  validation {
    condition     = !var.service_enabled || (var.vm_id == null ? false : var.vm_id >= 9100 && var.vm_id <= 999999999 && floor(var.vm_id) == var.vm_id)
    error_message = "An enabled persistent service VM requires an integer VM ID >= 9100."
  }
}

variable "template_vm_id" {
  description = "Reviewed, credential-free Debian/Ubuntu cloud-init template ID."
  type        = number
  default     = null
  validation {
    condition     = !var.service_enabled || (var.template_vm_id == null ? false : var.template_vm_id != var.vm_id)
    error_message = "An enabled service requires a distinct reviewed template VM ID."
  }
}

variable "vm_name" {
  description = "Persistent control VM name."
  type        = string
  default     = ""
}

variable "node_name" {
  description = "Reviewed Proxmox node name."
  type        = string
  default     = ""
}

variable "pool_id" {
  description = "Dedicated persistent-service pool, not the disposable lab pool."
  type        = string
  default     = ""
}

variable "boot_datastore_id" {
  description = "Approved persistent datastore for the full-clone boot disk."
  type        = string
  default     = ""
}

variable "data_datastore_id" {
  description = "Approved persistent datastore for the separate PostgreSQL/data disk."
  type        = string
  default     = ""
}

variable "data_disk_gb" {
  description = "Capacity of the dedicated protected data disk in GiB."
  type        = number
  default     = 250
  validation {
    condition     = var.data_disk_gb >= 250 && var.data_disk_gb <= 4096 && floor(var.data_disk_gb) == var.data_disk_gb
    error_message = "The persistent data disk must be an integer between 250 and 4096 GiB."
  }
}

variable "cores" {
  type    = number
  default = 4
  validation {
    condition     = var.cores >= 2 && var.cores <= 16 && floor(var.cores) == var.cores
    error_message = "The service VM requires 2-16 whole vCPUs."
  }
}

variable "memory_mb" {
  type    = number
  default = 16384
  validation {
    condition     = var.memory_mb >= 8192 && var.memory_mb <= 65536 && floor(var.memory_mb) == var.memory_mb
    error_message = "The service VM requires 8192-65536 MiB RAM."
  }
}

variable "management_bridge" {
  description = "Approved private management bridge; never a disposable lab or personal bridge."
  type        = string
  default     = ""
}

variable "ipv4_address" {
  description = "Approved static private IPv4 CIDR for control-plane clients."
  type        = string
  default     = ""
}

variable "ipv4_gateway" {
  description = "Approved management gateway, if required."
  type        = string
  default     = ""
}

variable "dns_servers" {
  description = "Approved DNS resolvers reachable only through the reviewed management segment."
  type        = list(string)
  default     = []
}

variable "ssh_public_keys" {
  description = "Operator-approved SSH public keys; no password is injected."
  type        = list(string)
  default     = []
}

variable "admin_user" {
  description = "Operator login created by the reviewed cloud-init template."
  type        = string
  default     = ""
}

variable "start_after_review" {
  description = "Keep the first plan stopped; only start after network, backups, and playbook review."
  type        = bool
  default     = false
}
