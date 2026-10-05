terraform {
  # Existing ephemeral provider resources require OpenTofu 1.11 or newer.
  required_version = ">= 1.11.0, < 2.0.0"

  encryption {
    key_provider "pbkdf2" "state" {
      passphrase = var.state_encryption_passphrase
    }

    method "aes_gcm" "state" {
      keys = key_provider.pbkdf2.state
    }

    state {
      method   = method.aes_gcm.state
      enforced = true
    }

    plan {
      method   = method.aes_gcm.state
      enforced = true
    }
  }

  # Initialize only with a reviewed, encrypted off-VM backend configuration.
  # No local production state is permitted for this persistent service VM.
  backend "s3" {}

  required_providers {
    proxmox = {
      source  = "bpg/proxmox"
      version = "~> 0.83.1"
    }
    vault = {
      source  = "hashicorp/vault"
      version = "~> 5.11.0"
    }
  }
}
