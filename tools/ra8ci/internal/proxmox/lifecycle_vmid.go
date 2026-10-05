// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package proxmox

// LifecycleVMIDAllowed reports whether a reservation uses the sub-range
// assigned to the OpenTofu lab guest lifecycle.
func LifecycleVMIDAllowed(vmid int) bool {
	return vmid >= 9020 && vmid <= 9039
}
