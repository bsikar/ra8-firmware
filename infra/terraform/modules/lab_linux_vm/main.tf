resource "proxmox_virtual_environment_vm" "this" {
  name      = var.name
  node_name = var.node_name
  vm_id     = var.vm_id

  description = "Disposable RA8 Terraform lab VM; RA8_LAB_RUN=${var.run_id}"
  tags        = ["terraform", "ra8-lab", "run-${var.run_id}"]
  pool_id     = var.pool_id
  started     = var.started
  on_boot     = false
  protection  = true

  # The clone template must already exist. This module never downloads an
  # image, changes a template, or manages Proxmox host storage.
  clone {
    vm_id        = var.template_vm_id
    datastore_id = var.datastore_id
    full         = true
  }

  agent {
    enabled = false
  }

  cpu {
    cores = var.cores
    type  = "host"
  }

  memory {
    dedicated = var.memory_mb
  }

  network_device {
    bridge       = var.bridge
    firewall     = true
    disconnected = !var.network_enabled
    # Keep an accidentally exposed lab guest from saturating the segment.
    rate_limit = 10
  }

  initialization {
    datastore_id = var.datastore_id
    # The Debian template already carries its cloud-init drive on ide2.
    # Pin the interface so Terraform converges without trying to remove that
    # protected drive after cloning.
    interface = "ide2"

    ip_config {
      ipv4 {
        address = var.ipv4_address
        gateway = var.ipv4_gateway
      }

    }

    dns {
      # The host firewall permits DNS only to this public resolver. No guest
      # can use the Proxmox host or the production LAN as a resolver.
      servers = ["1.1.1.1"]
    }

    user_account {
      keys     = var.ssh_public_keys
      username = var.user_name
    }
  }

  lifecycle {
    prevent_destroy = true

    precondition {
      condition     = lower(trimspace(var.bridge)) == "vmbr8" || lower(trimspace(var.bridge)) == "vmbr9"
      error_message = "The lab VM must attach only to the dedicated lab bridge (vmbr8 or vmbr9)."
    }

    precondition {
      condition     = lower(trimspace(var.pool_id)) == "ra8-tf-lab"
      error_message = "The lab VM must belong to the exact dedicated ra8-tf-lab pool."
    }

    precondition {
      condition     = lower(trimspace(var.datastore_id)) == "ra8-tf-lab"
      error_message = "The lab VM must clone into the exact dedicated ra8-tf-lab datastore."
    }

    precondition {
      condition     = var.cores >= 1 && var.cores <= 4 && var.memory_mb >= 512 && var.memory_mb <= 8192
      error_message = "The lab VM is limited to 1-4 cores and 512-8192 MB of memory."
    }

    precondition {
      condition     = can(regex("^[0-9a-f]{16}$", trimspace(var.run_id)))
      error_message = "The lab VM run_id must be exactly 16 lowercase hexadecimal characters."
    }

    precondition {
      condition     = !var.network_enabled || var.started
      error_message = "The lab VM cannot enable its network while stopped."
    }

    precondition {
      condition = (
        (!var.network_enabled && lower(trimspace(var.ipv4_address)) == "dhcp" && var.ipv4_gateway == null) ||
        (var.network_enabled && can(regex("^10\\.250\\.[89]\\.[0-9]{1,3}/24$", trimspace(var.ipv4_address))) && (var.ipv4_gateway == "10.250.8.1" || var.ipv4_gateway == "10.250.9.1"))
      )
      error_message = "A disconnected lab VM must use DHCP without a gateway; an enabled lab VM must use the exact lab network (10.250.8.0/24 or 10.250.9.0/24)."
    }

    precondition {
      condition     = length(var.ssh_public_keys) > 0
      error_message = "The lab VM must have at least one SSH public key and no password authentication configured."
    }
  }
}
