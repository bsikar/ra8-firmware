terraform {
  required_version = ">= 1.10.0, < 2.0.0"

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
