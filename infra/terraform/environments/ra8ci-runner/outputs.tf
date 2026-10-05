output "vm_id" {
  value = var.runner_enabled ? module.runner[0].vm_id : null
}

output "name" {
  value = var.runner_enabled ? module.runner[0].name : null
}

output "reservation_id" {
  value = var.runner_enabled ? module.runner[0].reservation_id : null
}
