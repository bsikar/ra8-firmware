output "control_vm" {
  description = "Nonsecret identity for the separate Ansible inventory and operations record."
  value = var.service_enabled ? {
    vm_id        = proxmox_virtual_environment_vm.control[0].vm_id
    name         = proxmox_virtual_environment_vm.control[0].name
    node_name    = proxmox_virtual_environment_vm.control[0].node_name
    ipv4_address = var.ipv4_address
  } : null
}
