output "lab_vm_id" {
  description = "ID of the managed lab VM, when enabled."
  value       = try(module.lab_vm[0].vm_id, null)
}

output "lab_container_id" {
  description = "ID of the managed lab container, when enabled."
  value       = try(module.lab_container[0].container_id, null)
}
