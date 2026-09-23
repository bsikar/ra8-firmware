variable "creation_operation_id" {
  description = "Exact durable ra8ci operation UUID embedded in the guest marker."
  type        = string

  validation {
    condition     = can(regex("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$", var.creation_operation_id))
    error_message = "creation_operation_id must be a canonical lowercase UUID."
  }
}

variable "reservation_id" {
  description = "Durable ra8ci reservation UUID embedded in the guest marker."
  type        = string

  validation {
    condition     = can(regex("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$", var.reservation_id))
    error_message = "reservation_id must be a canonical lowercase UUID."
  }
}

variable "run_id" {
  description = "Opaque 16-hex ra8ci run marker."
  type        = string

  validation {
    condition     = can(regex("^[0-9a-f]{16}$", var.run_id))
    error_message = "run_id must be exactly 16 lowercase hexadecimal characters."
  }
}

variable "vm_id" {
  description = "Explicit disposable VMID from the reviewed runner allowlist."
  type        = number

  validation {
    condition     = var.vm_id >= 9000 && var.vm_id <= 9099 && floor(var.vm_id) == var.vm_id
    error_message = "vm_id must be an integer in the disposable 9000-9099 range."
  }
}

variable "template_vm_id" {
  description = "Exact pre-created Linux runner template VMID."
  type        = number

  validation {
    condition     = var.template_vm_id >= 9000 && var.template_vm_id <= 9099 && floor(var.template_vm_id) == var.template_vm_id
    error_message = "template_vm_id must be an integer in the reviewed 9000-9099 range."
  }
}

variable "node_name" {
  description = "Explicit Proxmox node approved for disposable CI capacity."
  type        = string

  validation {
    condition     = can(regex("^[A-Za-z0-9][A-Za-z0-9_.-]{0,63}$", var.node_name))
    error_message = "node_name must be a simple explicit identifier."
  }
}

variable "pool_id" {
  description = "The dedicated disposable pool."
  type        = string
  default     = "ra8-tf-lab"

  validation {
    condition     = lower(trimspace(var.pool_id)) == "ra8-tf-lab"
    error_message = "Runner VMs must belong to the exact ra8-tf-lab pool."
  }
}

variable "datastore_id" {
  description = "The dedicated disposable datastore."
  type        = string
  default     = "ra8-tf-lab"

  validation {
    condition     = lower(trimspace(var.datastore_id)) == "ra8-tf-lab"
    error_message = "Runner VMs must use the exact ra8-tf-lab datastore."
  }
}

variable "bridge" {
  description = "Reviewed isolated Linux lab bridge."
  type        = string

  validation {
    condition     = lower(trimspace(var.bridge)) == "vmbr8" || lower(trimspace(var.bridge)) == "vmbr9"
    error_message = "Runner VMs may use only vmbr8 or vmbr9."
  }
}

variable "cores" {
  description = "Bounded runner vCPU count."
  type        = number
  default     = 2

  validation {
    condition     = var.cores >= 1 && var.cores <= 4 && floor(var.cores) == var.cores
    error_message = "Runner VMs are limited to 1-4 vCPUs."
  }
}

variable "memory_mb" {
  description = "Bounded runner memory."
  type        = number
  default     = 4096

  validation {
    condition     = var.memory_mb >= 512 && var.memory_mb <= 8192 && floor(var.memory_mb) == var.memory_mb
    error_message = "Runner VMs are limited to 512-8192 MB."
  }
}

variable "ipv4_address" {
  description = "Static address on the isolated CI segment."
  type        = string

  validation {
    condition     = can(regex("^10\\.250\\.[89]\\.[0-9]{1,3}/24$", trimspace(var.ipv4_address)))
    error_message = "Runner VMs must use an address in 10.250.8.0/24 or 10.250.9.0/24."
  }
}

variable "ipv4_gateway" {
  description = "Gateway on the isolated CI segment."
  type        = string

  validation {
    condition     = var.ipv4_gateway == "10.250.8.1" || var.ipv4_gateway == "10.250.9.1"
    error_message = "Runner VMs must use the exact gateway of their isolated segment."
  }
}

variable "ssh_public_keys" {
  description = "Ephemeral, server-controlled SSH key used only by the guest bootstrap channel."
  type        = list(string)

  validation {
    condition     = length(var.ssh_public_keys) == 1 && length(var.ssh_public_keys[0]) <= 1024 && can(regex("^(ssh-ed25519|ecdsa-sha2-nistp256) ", var.ssh_public_keys[0]))
    error_message = "Exactly one bounded Ed25519 or P-256 SSH public key is required."
  }
}

variable "user_name" {
  description = "Non-root cloud-init bootstrap identity."
  type        = string
  default     = "ra8ci"

  validation {
    condition     = can(regex("^[a-z_][a-z0-9_-]{0,31}$", var.user_name))
    error_message = "user_name must be a non-root Unix account name."
  }
}

variable "started" {
  description = "Desired guest power state, changed only by the lifecycle controller."
  type        = bool
  default     = false
}

variable "network_enabled" {
  description = "Whether the isolated lab NIC is connected."
  type        = bool
  default     = false
}
