variable "state_encryption_passphrase" {
  description = "Protected controller-side key for this encrypted, disposable lifecycle state."
  type        = string
  sensitive   = true
  nullable    = false
}

variable "proxmox_endpoint" {
  description = "Controller-local loopback proxy endpoint for the Proxmox API."
  type        = string
  nullable    = false
}

variable "proxmox_insecure" {
  description = "TLS verification setting for the controller-local API tunnel."
  type        = bool
  default     = true
}

variable "openbao_address" {
  description = "Runtime OpenBao address supplied by the protected controller configuration."
  type        = string
  nullable    = false
}

variable "openbao_role_id" {
  description = "Runtime AppRole ID supplied by protected controller storage."
  type        = string
  sensitive   = true
  nullable    = false
}

variable "openbao_secret_id" {
  description = "Runtime AppRole secret ID supplied by protected controller storage."
  type        = string
  sensitive   = true
  nullable    = false
}

variable "openbao_auth_path" {
  type    = string
  default = "auth/approle/login"
}

variable "openbao_kv_mount" {
  type    = string
  default = "secret"
}

variable "openbao_secret_path" {
  type    = string
  default = "terraform/proxmox-lab"
}

variable "node_name" {
  type    = string
  default = "pve1"

  validation {
    condition     = var.node_name == "pve1"
    error_message = "This disposable entrypoint is allowlisted only for node pve1."
  }
}

variable "run_id" {
  description = "Sixteen lowercase hexadecimal characters generated for one create/destroy run."
  type        = string

  validation {
    condition     = can(regex("^[0-9a-f]{16}$", var.run_id))
    error_message = "run_id must be exactly 16 lowercase hexadecimal characters."
  }
}

variable "vm_id" {
  type    = number
  default = 9020

  validation {
    condition     = var.vm_id == 9020 && var.vm_id >= 9000 && var.vm_id <= 9099
    error_message = "This lifecycle uses its assigned VMID 9020 inside the reserved 9000-9099 range."
  }
}

variable "template_vm_id" {
  type    = number
  default = 9001

  validation {
    condition     = var.template_vm_id == 9001
    error_message = "This lifecycle may clone only the reviewed Debian template 9001."
  }
}

variable "template_name" {
  type    = string
  default = "ra8-lab-debian-template"

  validation {
    condition     = var.template_name == "ra8-lab-debian-template"
    error_message = "Template 9001 must have the exact reviewed Debian template name."
  }
}

variable "bridge" {
  type    = string
  default = "vmbr9"

  validation {
    condition     = var.bridge == "vmbr9"
    error_message = "The guest must use vmbr9 created by the per-run lab recipe."
  }
}

variable "pool_id" {
  type    = string
  default = "ra8-tf-lab"

  validation {
    condition     = var.pool_id == "ra8-tf-lab"
    error_message = "The guest must use the exact ra8-tf-lab resource pool."
  }
}

variable "datastore_id" {
  type    = string
  default = "ra8-tf-lab"

  validation {
    condition     = var.datastore_id == "ra8-tf-lab"
    error_message = "The guest must use the exact ra8-tf-lab datastore."
  }
}

variable "ipv4_address" {
  description = "Static address derived from VMID 9020 on the recipe-created lab bridge."
  type        = string
  default     = "10.250.9.30/24"

  validation {
    condition     = var.ipv4_address == "10.250.9.30/24"
    error_message = "VMID 9020 is pinned to its derived lab address."
  }
}

variable "guest_username" {
  description = "The precreated non-root account used for the recipe's SSH readiness probe."
  type        = string
  default     = "terraform-lab"

  validation {
    condition     = var.guest_username == "terraform-lab"
    error_message = "The disposable guest must use the reviewed terraform-lab account."
  }
}

variable "ssh_public_key" {
  description = "Per-run public key; its private half remains in the protected controller state directory."
  type        = string
  nullable    = false

  validation {
    condition     = can(regex("^ssh-ed25519 [A-Za-z0-9+/]+={0,3}( .*)?$", var.ssh_public_key))
    error_message = "A valid per-run Ed25519 public key is required for the guest readiness probe."
  }
}
