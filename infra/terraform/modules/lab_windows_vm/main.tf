resource "proxmox_virtual_environment_vm" "this" {
  name      = var.name
  node_name = var.node_name
  vm_id     = var.vm_id

  description = "Disposable RA8 Terraform lab Windows Server VM; RA8_LAB_RUN=${var.run_id}"
  tags        = ["terraform", "ra8-lab", "windows", "run-${var.run_id}"]
  pool_id     = var.pool_id
  started     = var.started
  on_boot     = false
  protection  = true

  # The Windows template must already contain Windows, VirtIO support,
  # Cloudbase-Init, and the reviewed WinRM policy. This module never downloads
  # an ISO, runs Sysprep, changes a template, or manages host storage.
  clone {
    vm_id        = var.template_vm_id
    datastore_id = var.datastore_id
    full         = true
  }

  operating_system {
    # Proxmox's closest generic Windows type for current Server templates;
    # this does not install or select the Windows Server release.
    type = "win10"
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
    rate_limit   = 10
  }

  # Cloudbase-Init consumes the Proxmox metadata disk. Credentials are not
  # injected here; the template and the later Ansible run own that boundary.
  initialization {
    datastore_id = var.datastore_id
    interface    = "ide2"

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
      error_message = "The Windows lab VM must attach only to the dedicated lab bridge (vmbr8 or vmbr9)."
    }

    precondition {
      condition     = lower(trimspace(var.pool_id)) == "ra8-tf-lab"
      error_message = "The Windows lab VM must belong to the exact dedicated ra8-tf-lab pool."
    }

    precondition {
      condition     = lower(trimspace(var.datastore_id)) == "ra8-tf-lab"
      error_message = "The Windows lab VM must clone into the exact dedicated ra8-tf-lab datastore."
    }

    precondition {
      condition     = var.cores >= 2 && var.cores <= 4 && var.memory_mb >= 4096 && var.memory_mb <= 8192
      error_message = "The Windows lab VM is limited to 2-4 cores and 4096-8192 MB of memory."
    }

    precondition {
      condition     = can(regex("^[0-9a-f]{16}$", trimspace(var.run_id)))
      error_message = "The Windows lab VM run_id must be exactly 16 lowercase hexadecimal characters."
    }

    precondition {
      condition     = !var.network_enabled || var.started
      error_message = "The Windows lab VM cannot enable its network while stopped."
    }

    precondition {
      condition = (
        (!var.network_enabled && lower(trimspace(var.ipv4_address)) == "dhcp" && var.ipv4_gateway == null) ||
        (var.network_enabled && can(regex("^10\\.250\\.[89]\\.[0-9]{1,3}/24$", trimspace(var.ipv4_address))) && (var.ipv4_gateway == "10.250.8.1" || var.ipv4_gateway == "10.250.9.1"))
      )
      error_message = "A disconnected Windows lab VM must use DHCP without a gateway; an enabled lab VM must use an approved lab network (10.250.8.0/24 or 10.250.9.0/24) and matching gateway."
    }
  }
}
