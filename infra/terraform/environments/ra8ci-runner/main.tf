module "runner" {
  source = "../../modules/ra8ci_ephemeral_runner"

  reservation_id        = var.runner.reservation_id
  creation_operation_id = var.runner.creation_operation_id
  run_id                = var.runner.run_id
  vm_id                 = var.runner.vm_id
  template_vm_id        = var.runner.template_vm_id
  node_name             = var.runner.node_name
  pool_id               = var.runner.pool_id
  datastore_id          = var.runner.datastore_id
  bridge                = var.runner.bridge
  cores                 = var.runner.cores
  memory_mb             = var.runner.memory_mb
  ipv4_address          = var.runner.ipv4_address
  ipv4_gateway          = var.runner.ipv4_gateway
  ssh_public_keys       = var.runner.ssh_public_keys
  user_name             = var.runner.user_name
  started               = var.runner.started
  network_enabled       = var.runner.network_enabled
}
