# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie

# Dedicated Proxmox lab cache container (apt-cacher-ng & artifact cache)
resource "proxmox_virtual_environment_container" "cache" {
  count = var.enabled && var.template_ct_id != "" ? 1 : 0

  node_name   = var.node_name
  vm_id       = var.vm_id
  description = "RA8 Lab Cache (apt-cacher-ng & artifact HTTP cache)"
  pool_id     = var.pool_id
  tags        = ["terraform", "ra8-lab", "cache"]

  cpu {
    cores = var.cores
  }

  memory {
    dedicated = var.memory_mb
    swap      = 512
  }

  disk {
    datastore_id = var.datastore_id
    size         = var.disk_size_gb
  }

  initialization {
    hostname = "ra8-lab-cache"

    ip_config {
      ipv4 {
        address = var.ipv4_address
        gateway = var.ipv4_gateway
      }
    }
  }

  network_interface {
    name    = "eth0"
    bridge  = var.bridge
    enabled = true
  }

  started       = true
  start_on_boot = true
  unprivileged  = true
}
