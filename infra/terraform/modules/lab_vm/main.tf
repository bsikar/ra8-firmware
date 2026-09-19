resource "proxmox_virtual_environment_vm" "this" {
  name      = var.name
  node_name = var.node_name
  vm_id     = var.vm_id

  description = "Disposable RA8 Terraform lab VM; managed by Terraform."
  tags        = ["terraform", "ra8-lab"]
  pool_id     = var.pool_id
  started     = false
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
    disconnected = true
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

      ipv6 {
        address = "manual"
      }
    }

    user_account {
      keys     = var.ssh_public_keys
      username = var.user_name
    }
  }

  lifecycle {
    prevent_destroy = true

    precondition {
      condition     = lower(trimspace(var.bridge)) == "vmbr9"
      error_message = "The lab VM must attach only to the exact pre-created vmbr9 lab bridge."
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
      condition     = lower(trimspace(var.ipv4_address)) == "dhcp" && var.ipv4_gateway == null
      error_message = "The lab VM must use DHCP with no configured gateway until the isolated lab network is reviewed."
    }

    precondition {
      condition     = length(var.ssh_public_keys) > 0
      error_message = "The lab VM must have at least one SSH public key and no password authentication configured."
    }
  }
}
