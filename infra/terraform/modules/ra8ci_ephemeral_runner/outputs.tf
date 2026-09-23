output "vm_id" {
  description = "The explicit disposable VMID."
  value       = proxmox_virtual_environment_vm.runner.vm_id
}

output "name" {
  description = "The deterministic runner VM name."
  value       = proxmox_virtual_environment_vm.runner.name
}

output "reservation_id" {
  description = "The durable ra8ci reservation marker."
  value       = var.reservation_id
}
