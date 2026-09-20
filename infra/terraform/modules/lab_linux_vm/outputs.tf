output "vm_id" {
  description = "The Proxmox ID of the lab VM."
  value       = proxmox_virtual_environment_vm.this.vm_id
}
