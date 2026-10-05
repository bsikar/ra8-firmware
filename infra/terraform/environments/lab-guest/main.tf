data "proxmox_virtual_environment_vm" "template" {
  node_name = var.node_name
  vm_id     = var.template_vm_id
}

resource "proxmox_virtual_environment_vm" "guest" {
  name        = "ra8-lab-${var.guest_profile}-${var.run_id}"
  node_name   = var.node_name
  vm_id       = var.vm_id
  description = "Disposable RA8 CI lifecycle guest; RA8_LAB_RUN=${var.run_id}"
  tags        = ["terraform", "ra8-lab", "run-${var.run_id}"]
  pool_id     = var.pool_id
  started     = true
  on_boot     = false
  protection  = false

  clone {
    vm_id        = var.template_vm_id
    datastore_id = var.datastore_id
    full         = true
  }

  cpu {
    cores = 4
    type  = "host"
  }

  memory {
    dedicated = 8192
  }

  disk {
    datastore_id = var.datastore_id
    # Windows 9011 already has its 64 GiB boot disk on sata0. Manage that
    # cloned disk in place instead of attaching a second 64 GiB disk.
    interface    = var.guest_profile == "windows" ? "sata0" : "scsi0"
    size         = var.disk_size_gb
    discard      = "on"
    iothread     = true
  }

  network_device {
    bridge       = var.bridge
    firewall     = true
    disconnected = false
    rate_limit   = 10
  }

  initialization {
    datastore_id = var.datastore_id
    interface    = "ide2"

    ip_config {
      ipv4 {
        address = var.ipv4_address
        gateway = null
      }
    }

    user_account {
      username = var.guest_username
      keys     = [var.ssh_public_key]
    }
  }

  lifecycle {
    precondition {
      condition = (
        data.proxmox_virtual_environment_vm.template.name == var.template_name &&
        data.proxmox_virtual_environment_vm.template.template &&
        data.proxmox_virtual_environment_vm.template.status == "stopped"
      )
      error_message = "Clone source must be the stopped, named template assigned to the selected profile."
    }

    precondition {
      condition     = var.bridge == "vmbr9" && var.pool_id == "ra8-tf-lab" && var.datastore_id == "ra8-tf-lab"
      error_message = "The guest network, pool, and datastore must match the disposable lab allowlist."
    }

    precondition {
      condition = (
        (var.guest_profile == "windows" ? var.vm_id == 9021 : (var.vm_id >= 9020 && var.vm_id <= 9039 && var.vm_id != 9021)) &&
        var.template_vm_id == (var.guest_profile == "windows" ? 9011 : 9001)
      )
      error_message = "The guest and template VMIDs must match the selected profile inside the reserved range."
    }

  }
}

output "vm_id" {
  value = proxmox_virtual_environment_vm.guest.vm_id
}

output "guest_name" {
  value = proxmox_virtual_environment_vm.guest.name
}
