# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie

output "cache_container_id" {
  description = "VMID of the cache container, if deployed."
  value       = try(proxmox_virtual_environment_container.cache[0].vm_id, null)
}

output "apt_cache_url" {
  description = "URL for the apt-cacher-ng proxy."
  value       = "http://${split("/", var.ipv4_address)[0]}:3142"
}

output "artifact_cache_url" {
  description = "URL for the Nginx artifact cache."
  value       = "http://${split("/", var.ipv4_address)[0]}:8080"
}
