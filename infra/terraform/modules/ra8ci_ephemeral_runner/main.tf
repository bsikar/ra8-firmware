locals {
  vm_name = "ra8-lab-ci-${var.vm_id}"
}

resource "proxmox_virtual_environment_vm" "runner" {
  name      = local.vm_name
  node_name = var.node_name
  vm_id     = var.vm_id

  description = "RA8CI_RESERVATION=${var.reservation_id};RA8CI_OPERATION=${var.creation_operation_id}"
  tags        = ["terraform", "ra8-lab", "ra8ci-runner", "reservation-${var.reservation_id}", "run-${var.run_id}"]
  pool_id     = var.pool_id
  started     = var.started
  on_boot     = false
  protection  = false

  # This module manages only a pre-created template and an explicitly assigned
  # disposable VMID. It never uploads images or changes host storage/networking.
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
    rate_limit   = 10
  }

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
      servers = ["1.1.1.1"]
    }

    user_account {
      keys     = var.ssh_public_keys
      username = var.user_name
    }
  }

  lifecycle {
    precondition {
      condition     = var.vm_id != var.template_vm_id
      error_message = "A runner reservation must never target its own template."
    }

    precondition {
      condition     = !var.network_enabled || var.started
      error_message = "The isolated runner NIC may be connected only while the VM is started."
    }

    precondition {
      condition     = (startswith(var.ipv4_address, "10.250.8.") && var.ipv4_gateway == "10.250.8.1") || (startswith(var.ipv4_address, "10.250.9.") && var.ipv4_gateway == "10.250.9.1")
      error_message = "Runner address and gateway must belong to the same isolated segment."
    }
  }
}
