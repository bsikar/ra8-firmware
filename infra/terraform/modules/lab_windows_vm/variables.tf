variable "name" {
  type = string
}

variable "run_id" {
  type = string
}

variable "vm_id" {
  type = number
}

variable "template_vm_id" {
  type = number
}

variable "pool_id" {
  type = string
}

variable "datastore_id" {
  type = string
}

variable "node_name" {
  type = string
}

variable "cores" {
  type = number
}

variable "memory_mb" {
  type = number
}

variable "bridge" {
  type = string
}

variable "ipv4_address" {
  type = string
}

variable "ipv4_gateway" {
  type     = string
  nullable = true
}

variable "started" {
  type = bool
}

variable "network_enabled" {
  type = bool
}

variable "ssh_public_keys" {
  type    = list(string)
  default = []
}

variable "user_name" {
  type    = string
  default = "Administrator"
}
