//! The M85 Module Manager's `_txm_module_manager_external_memory_enable`
//! (RA8FW-527), in place of the vendored cortex_m33 port's C. Upstream's
//! checks, in upstream's order and with upstream's statuses, then one more:
//! a grant shared_grant.zig refuses returns TXM_MODULE_INVALID_MEMORY, the
//! closest upstream status (there is no TX_INVALID_MEMORY_REGION). The
//! register writes are upstream's: base | sanitized attributes | XN, then
//! limit | attribute index | enable.
const c = @cImport({
    @cInclude("txm_module.h");
});
const grant = @import("shared_grant.zig");

export fn _txm_module_manager_external_memory_enable(
    module_instance: ?*c.TXM_MODULE_INSTANCE,
    start_address: ?*anyopaque,
    length: c.ULONG,
    attributes: c.UINT,
) c.UINT {
    if (c._txm_module_manager_ready != c.TX_TRUE) return c.TX_NOT_AVAILABLE;
    const instance = module_instance orelse return c.TX_PTR_ERROR;
    _ = c._tx_mutex_get(&c._txm_module_manager_mutex, c.TX_WAIT_FOREVER);
    defer _ = c._tx_mutex_put(&c._txm_module_manager_mutex);
    return enable(instance, @intCast(@intFromPtr(start_address)), length, attributes);
}

fn enable(instance: *c.TXM_MODULE_INSTANCE, address: u32, length: u32, attributes: u32) c.UINT {
    if (instance.txm_module_instance_id != c.TXM_MODULE_ID) return c.TX_PTR_ERROR;
    if (instance.txm_module_instance_state != c.TXM_MODULE_LOADED) return c.TX_START_ERROR;
    const count = instance.txm_module_instance_shared_memory_count;
    if (count >= c.TXM_MODULE_MPU_SHARED_ENTRIES) return c.TX_NO_MEMORY;
    if (address & (c.TXM_MODULE_MPU_ALIGNMENT - 1) != 0) return c.TXM_MODULE_ALIGNMENT_ERROR;
    if (grant.check(address, length) != .allowed) return c.TXM_MODULE_INVALID_MEMORY;

    const entry = &instance.txm_module_instance_mpu_registers[@as(usize, c.TXM_MODULE_MPU_SHARED_INDEX) + count];
    entry.txm_module_mpu_region_base_address = address | (attributes & c.TXM_MODULE_ATTRIBUTE_MASK) | c.TXM_MODULE_ATTRIBUTE_EXECUTE_NEVER;
    entry.txm_module_mpu_region_limit_address = (address + length - 1) | c.TXM_MODULE_ATTRIBUTE_INDEX | c.TXM_MODULE_ATTRIBUTE_REGION_ENABLE;
    instance.txm_module_instance_shared_memory_address[count] = address;
    instance.txm_module_instance_shared_memory_length[count] = length;
    instance.txm_module_instance_shared_memory_count = count + 1;
    return c.TX_SUCCESS;
}
