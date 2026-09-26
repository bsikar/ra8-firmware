output "lab_linux_vm_id" {
  description = "ID of the managed Linux lab VM, when enabled."
  value       = try(module.lab_linux_vm[0].vm_id, null)
}

output "lab_vm_id" {
  description = "Deprecated alias for lab_linux_vm_id."
  value       = try(module.lab_linux_vm[0].vm_id, null)
}

output "lab_windows_vm_id" {
  description = "ID of the managed Windows lab VM, when enabled."
  value       = try(module.lab_windows_vm[0].vm_id, null)
}

output "lab_container_id" {
  description = "ID of the managed lab container, when enabled."
  value       = try(module.lab_container[0].container_id, null)
}
