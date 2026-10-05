// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package proxmox

import "testing"

func TestLifecycleIdentityMarkerCarriesTheLabRun(t *testing.T) {
	identity := Identity{ReservationID: "0192f3a4-b5c6-7d8e-9f01-1234567890ab",
		CreationOperationID: "0192f3a4-b5c6-7d8e-9f01-1234567890ac", RunID: "0000000000003039"}
	want := "RA8CI_RESERVATION=" + identity.ReservationID + ";RA8CI_OPERATION=" +
		identity.CreationOperationID + ";RA8_LAB_RUN=0000000000003039"
	if identity.marker() != want {
		t.Fatalf("run marker = %q, want %q", identity.marker(), want)
	}
}

func TestLifecycleVMIDRangeRefusesOutsideIDs(t *testing.T) {
	for _, vmid := range []int{9019, 9040, 9000, 9099} {
		if LifecycleVMIDAllowed(vmid) {
			t.Errorf("VMID %d is outside the lifecycle range and must be refused", vmid)
		}
	}
	for _, vmid := range []int{9020, 9039} {
		if !LifecycleVMIDAllowed(vmid) {
			t.Errorf("VMID %d is inside the lifecycle range and must be accepted", vmid)
		}
	}
}
