resource "proxmox_virtual_environment_container" "this" {
  node_name = var.node_name
  vm_id     = var.vm_id

  description   = "Disposable RA8 Terraform lab container; managed by Terraform."
  start_on_boot = false
  started       = false
  protection    = true
  unprivileged  = true
  pool_id       = var.pool_id
  tags          = ["terraform", "ra8-lab"]

  cpu {
    cores = var.cores
  }

  memory {
    dedicated = var.memory_mb
    swap      = 0
  }

  # The template must already exist. This module never downloads or changes a
  # container template.
  clone {
    vm_id        = var.template_ct_id
    datastore_id = var.datastore_id
  }

  # Keep the container unprivileged and disable the nesting escape surface.
  features {
    nesting = false
  }

  initialization {
    hostname = var.hostname

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
      keys = var.ssh_public_keys
    }
  }

  network_interface {
    name     = "eth0"
    bridge   = var.bridge
    enabled  = false
    firewall = true
    # Keep an accidentally exposed lab guest from saturating the segment.
    rate_limit = 10
  }

  lifecycle {
    prevent_destroy = true

    precondition {
      condition     = lower(trimspace(var.bridge)) == "vmbr9"
      error_message = "The lab container must attach only to the exact pre-created vmbr9 lab bridge."
    }

    precondition {
      condition     = lower(trimspace(var.pool_id)) == "ra8-tf-lab"
      error_message = "The lab container must belong to the exact dedicated ra8-tf-lab pool."
    }

    precondition {
      condition     = lower(trimspace(var.datastore_id)) == "ra8-tf-lab"
      error_message = "The lab container must clone into the exact dedicated ra8-tf-lab datastore."
    }

    precondition {
      condition     = var.cores >= 1 && var.cores <= 2 && var.memory_mb >= 256 && var.memory_mb <= 4096
      error_message = "The lab container is limited to 1-2 cores and 256-4096 MB of memory."
    }

    precondition {
      condition     = lower(trimspace(var.ipv4_address)) == "dhcp" && var.ipv4_gateway == null
      error_message = "The lab container must use DHCP with no configured gateway until the isolated lab network is reviewed."
    }

    precondition {
      condition     = length(var.ssh_public_keys) > 0
      error_message = "The lab container must have at least one SSH public key and no password authentication configured."
    }
  }
}
