output "container_id" {
  description = "The Proxmox ID of the lab container."
  value       = proxmox_virtual_environment_container.this.vm_id
}
