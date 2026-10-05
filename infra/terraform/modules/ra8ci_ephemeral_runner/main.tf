locals {
  vm_name = "ra8-lab-ci-${var.vm_id}"
}

data "proxmox_virtual_environment_vm" "template" {
  node_name = var.node_name
  vm_id     = var.template_vm_id
}

resource "proxmox_virtual_environment_vm" "runner" {
  name      = local.vm_name
  node_name = var.node_name
  vm_id     = var.vm_id

  description = "RA8CI_RESERVATION=${var.reservation_id};RA8CI_OPERATION=${var.creation_operation_id};RA8_LAB_RUN=${var.run_id}"
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

  disk {
    datastore_id = var.datastore_id
    interface    = "scsi0"
    size         = 32
    discard      = "on"
    iothread     = true
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
      condition = (
        data.proxmox_virtual_environment_vm.template.name == var.template_name &&
        data.proxmox_virtual_environment_vm.template.template &&
        data.proxmox_virtual_environment_vm.template.status == "stopped" &&
        var.template_vm_id == 9001
      )
      error_message = "Runner clone requires the stopped, named Debian template 9001."
    }

    precondition {
      condition     = var.vm_id >= 9020 && var.vm_id <= 9039 && var.vm_id != var.template_vm_id
      error_message = "A runner reservation must use an assigned lifecycle VMID from 9020 through 9039."
    }

    precondition {
      condition     = var.node_name == "pve1" && var.pool_id == "ra8-tf-lab" && var.datastore_id == "ra8-tf-lab" && var.bridge == "vmbr9"
      error_message = "Runner guests must use pve1, the ra8-tf-lab pool and datastore, and vmbr9."
    }

    precondition {
      condition     = var.cores == 4 && var.memory_mb == 8192 && var.run_id != ""
      error_message = "Runner guests are capped at 4 vCPU, 8 GB and require a run marker."
    }

    precondition {
      condition     = !var.network_enabled || var.started
      error_message = "The isolated runner NIC may be connected only while the VM is started."
    }
  }
}
