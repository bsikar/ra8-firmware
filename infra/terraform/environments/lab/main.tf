locals {
  effective_linux_vm = var.lab_linux_vm.enabled ? var.lab_linux_vm : (var.lab_vm != null && var.lab_vm.enabled ? var.lab_vm : var.lab_linux_vm)
}

module "lab_linux_vm" {
  count  = var.lab_enabled && local.effective_linux_vm.enabled ? 1 : 0
  source = "../../modules/lab_linux_vm"

  name            = local.effective_linux_vm.name
  run_id          = local.effective_linux_vm.run_id
  vm_id           = local.effective_linux_vm.vm_id
  template_vm_id  = local.effective_linux_vm.template_vm_id
  pool_id         = local.effective_linux_vm.pool_id
  datastore_id    = local.effective_linux_vm.datastore_id
  node_name       = local.effective_linux_vm.node_name
  cores           = local.effective_linux_vm.cores
  memory_mb       = local.effective_linux_vm.memory_mb
  bridge          = local.effective_linux_vm.bridge
  ipv4_address    = local.effective_linux_vm.ipv4_address
  ipv4_gateway    = local.effective_linux_vm.ipv4_gateway
  ssh_public_keys = local.effective_linux_vm.ssh_public_keys
  user_name       = local.effective_linux_vm.user_name
  started         = local.effective_linux_vm.started
  network_enabled = local.effective_linux_vm.network_enabled
}

module "lab_windows_vm" {
  count  = var.lab_enabled && var.lab_windows_vm.enabled ? 1 : 0
  source = "../../modules/lab_windows_vm"

  name            = var.lab_windows_vm.name
  run_id          = var.lab_windows_vm.run_id
  vm_id           = var.lab_windows_vm.vm_id
  template_vm_id  = var.lab_windows_vm.template_vm_id
  pool_id         = var.lab_windows_vm.pool_id
  datastore_id    = var.lab_windows_vm.datastore_id
  node_name       = var.lab_windows_vm.node_name
  cores           = var.lab_windows_vm.cores
  memory_mb       = var.lab_windows_vm.memory_mb
  bridge          = var.lab_windows_vm.bridge
  ipv4_address    = var.lab_windows_vm.ipv4_address
  ipv4_gateway    = var.lab_windows_vm.ipv4_gateway
  ssh_public_keys = var.lab_windows_vm.ssh_public_keys
  user_name       = var.lab_windows_vm.user_name
  started         = var.lab_windows_vm.started
  network_enabled = var.lab_windows_vm.network_enabled
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
