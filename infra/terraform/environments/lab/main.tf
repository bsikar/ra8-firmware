module "lab_vm" {
  count  = var.lab_enabled && var.lab_vm.enabled ? 1 : 0
  source = "../../modules/lab_vm"

  name            = var.lab_vm.name
  vm_id           = var.lab_vm.vm_id
  template_vm_id  = var.lab_vm.template_vm_id
  pool_id         = var.lab_vm.pool_id
  datastore_id    = var.lab_vm.datastore_id
  node_name       = var.lab_vm.node_name
  cores           = var.lab_vm.cores
  memory_mb       = var.lab_vm.memory_mb
  bridge          = var.lab_vm.bridge
  ipv4_address    = var.lab_vm.ipv4_address
  ipv4_gateway    = var.lab_vm.ipv4_gateway
  ssh_public_keys = var.lab_vm.ssh_public_keys
  user_name       = var.lab_vm.user_name
}

module "lab_container" {
  count  = var.lab_enabled && var.allow_lxc && var.lab_container.enabled ? 1 : 0
  source = "../../modules/lab_container"

  hostname        = var.lab_container.hostname
  vm_id           = var.lab_container.vm_id
  template_ct_id  = var.lab_container.template_ct_id
  pool_id         = var.lab_container.pool_id
  datastore_id    = var.lab_container.datastore_id
  node_name       = var.lab_container.node_name
  cores           = var.lab_container.cores
  memory_mb       = var.lab_container.memory_mb
  bridge          = var.lab_container.bridge
  ipv4_address    = var.lab_container.ipv4_address
  ipv4_gateway    = var.lab_container.ipv4_gateway
  ssh_public_keys = var.lab_container.ssh_public_keys
}
