terraform {
  backend "http" {}

  required_version = ">= 1.10.0, < 2.0.0"

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
