# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie

variable "enabled" {
  description = "Whether the dedicated lab cache container is enabled."
  type        = bool
  default     = false
}

variable "node_name" {
  description = "Target Proxmox node name."
  type        = string
  default     = "pve1"
}

variable "vm_id" {
  description = "VM/Container ID for the lab cache."
  type        = number
  default     = 9050
}

variable "pool_id" {
  description = "Proxmox resource pool."
  type        = string
  default     = "ra8-tf-lab"
}

variable "datastore_id" {
  description = "Target storage pool for container rootfs."
  type        = string
  default     = "ra8-tf-lab"
}

variable "template_ct_id" {
  description = "Template container ID or image volume."
  type        = string
  default     = ""
}

variable "bridge" {
  description = "Bridge to attach cache network interface to."
  type        = string
  default     = "vmbr9"
}

variable "ipv4_address" {
  description = "Static IPv4 address and CIDR prefix for cache container."
  type        = string
  default     = "10.250.9.2/24"
}

variable "ipv4_gateway" {
  description = "IPv4 default gateway."
  type        = string
  default     = "10.250.9.1"
}

variable "cores" {
  description = "CPU core count for cache container."
  type        = number
  default     = 2
}

variable "memory_mb" {
  description = "Memory in MB for cache container."
  type        = number
  default     = 2048
}

variable "disk_size_gb" {
  description = "Disk size in GB for cache storage."
  type        = number
  default     = 32
}
