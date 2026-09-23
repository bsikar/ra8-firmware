resource "proxmox_virtual_environment_vm" "control" {
  count = var.service_enabled ? 1 : 0

  name      = var.vm_name
  node_name = var.node_name
  vm_id     = var.vm_id
  pool_id   = var.pool_id

  description = "Persistent ra8ci control VM; database and control-plane only; no CI jobs"
  tags        = ["terraform", "ra8ci-control", "persistent"]
  protection  = true
  started     = var.start_after_review
  on_boot     = var.start_after_review

  clone {
    vm_id        = var.template_vm_id
    datastore_id = var.boot_datastore_id
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

  # The cloned template owns scsi0 and ide2. This is a distinct, protected
  # PostgreSQL/state disk, not a transient CI workspace or host bind mount.
  disk {
    datastore_id = var.data_datastore_id
    interface    = "scsi1"
    size         = var.data_disk_gb
    serial       = "RA8CIDATA"
    backup       = true
  }

  network_device {
    bridge       = var.management_bridge
    firewall     = true
    disconnected = !var.start_after_review
  }

  initialization {
    datastore_id = var.boot_datastore_id
    interface    = "ide2"

    ip_config {
      ipv4 {
        address = var.ipv4_address
        gateway = var.ipv4_gateway
      }
    }

    dns {
      servers = var.dns_servers
    }

    user_account {
      username = var.admin_user
      keys     = var.ssh_public_keys
    }
  }

  lifecycle {
    prevent_destroy = true

    precondition {
      condition = (
        var.network_approved && var.off_vm_backup_approved &&
        trimspace(var.off_vm_backup_target) != "" &&
        trimspace(var.openbao_kv_mount) != "" && trimspace(var.openbao_secret_path) != ""
      )
      error_message = "A persistent service VM requires explicit network/backup approval and dedicated runtime OpenBao credentials."
    }

    precondition {
      condition = (
        can(regex("^ra8ci-[a-z0-9-]+$", var.vm_name)) &&
        trimspace(var.node_name) != "" && trimspace(var.pool_id) != "" &&
        lower(trimspace(var.pool_id)) != "ra8-tf-lab" &&
        trimspace(var.boot_datastore_id) != "" && trimspace(var.data_datastore_id) != "" &&
        lower(trimspace(var.boot_datastore_id)) != "ra8-tf-lab" &&
        lower(trimspace(var.data_datastore_id)) != "ra8-tf-lab"
      )
      error_message = "Name, node, and persistent pool/datastores must be explicitly approved and must not reuse the disposable lab."
    }

    precondition {
      condition = (
        trimspace(var.management_bridge) != "" &&
        !contains(["vmbr0", "vmbr1", "vmbr8", "vmbr9"], lower(trimspace(var.management_bridge))) &&
        can(cidrhost(var.ipv4_address, 0)) &&
        can(cidrhost("${var.ipv4_gateway}/32", 0)) &&
        length(var.dns_servers) > 0 &&
        length(var.ssh_public_keys) > 0 &&
        can(regex("^[a-z_][a-z0-9_-]{0,31}$", var.admin_user))
      )
      error_message = "An approved private management bridge/address/gateway/DNS and SSH-only admin identity are required."
    }
  }
}
