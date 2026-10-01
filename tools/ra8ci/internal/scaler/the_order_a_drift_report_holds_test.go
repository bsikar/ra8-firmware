// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package scaler

import (
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/proxmox"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// Drift is the list an operator reads by hand, one guest at a time, against a
// cluster they are looking at in another window. Two runs over the same
// disagreement have to name it in the same place, or the reader cannot tell a
// new drift from one they already judged. The unaccounted list is already held
// to that (TestReportsAreOrderedByVMID); these pin the same promise for drift,
// which is the half an operator is most likely to work through row by row.

// driftingPair observes two guests whose names disagree with the reservations
// claiming their VMIDs, handing them over in the order given.
func driftingPair(t *testing.T, vmids ...int) GuestReconciliation {
	t.Helper()
	observed := make([]proxmox.VM, 0, len(vmids))
	reservations := make([]store.RunnerVM, 0, len(vmids))
	for _, vmid := range vmids {
		observed = append(observed, reconcileGuest(vmid, "ra8-lab-ci-9999", "running"))
		reservations = append(reservations, reconcileReservation(
			"res-"+itoaVMID(vmid), vmid, "ra8-lab-ci-"+itoaVMID(vmid)))
	}
	report, err := ReconcileObservedGuests(observed, reservations)
	if err != nil {
		t.Fatalf("reconcile failed: %v", err)
	}
	return report
}

func itoaVMID(vmid int) string {
	digits := ""
	for vmid > 0 {
		digits = string(rune('0'+vmid%10)) + digits
		vmid /= 10
	}
	return digits
}

func driftedVMIDs(report GuestReconciliation) []int {
	order := make([]int, 0, len(report.Drifted))
	for _, drift := range report.Drifted {
		order = append(order, drift.VMID)
	}
	return order
}

func sameOrder(got []int, want ...int) bool {
	if len(got) != len(want) {
		return false
	}
	for index, vmid := range want {
		if got[index] != vmid {
			return false
		}
	}
	return true
}

// Two drifts observed newest-VMID-first are reported lowest first.
func TestDriftIsReportedByVMIDNotByObservationOrder(t *testing.T) {
	report := driftingPair(t, 9003, 9001)
	if got := driftedVMIDs(report); !sameOrder(got, 9001, 9003) {
		t.Fatalf("drifted = %v, want [9001 9003]", got)
	}
}

// The same set handed over in three different orders reports one order.
func TestDriftOrderDoesNotDependOnHowTheSetWasHandedOver(t *testing.T) {
	for _, given := range [][]int{{9002, 9000, 9004}, {9004, 9002, 9000}, {9000, 9002, 9004}} {
		report := driftingPair(t, given...)
		if got := driftedVMIDs(report); !sameOrder(got, 9000, 9002, 9004) {
			t.Fatalf("given %v, drifted = %v, want [9000 9002 9004]", given, got)
		}
	}
}

// Both lists are sorted in the same report, and neither borrows the other's
// ordering: an unaccounted guest between two drifted VMIDs does not displace
// them.
func TestDriftAndUnaccountedAreEachSortedInOneReport(t *testing.T) {
	report, err := ReconcileObservedGuests(
		[]proxmox.VM{
			reconcileGuest(9005, "ra8-lab-ci-9999", "running"),
			reconcileGuest(9004, "ra8-lab-ci-9004", "running"),
			reconcileGuest(9001, "ra8-lab-ci-9999", "stopped"),
			reconcileGuest(9002, "ra8-lab-ci-9002", "running"),
		},
		[]store.RunnerVM{
			reconcileReservation("res-9005", 9005, "ra8-lab-ci-9005"),
			reconcileReservation("res-9001", 9001, "ra8-lab-ci-9001"),
		})
	if err != nil {
		t.Fatalf("reconcile failed: %v", err)
	}
	if got := driftedVMIDs(report); !sameOrder(got, 9001, 9005) {
		t.Fatalf("drifted = %v, want [9001 9005]", got)
	}
	if len(report.Unaccounted) != 2 ||
		report.Unaccounted[0].Identity.VMID != 9002 || report.Unaccounted[1].Identity.VMID != 9004 {
		t.Fatalf("unaccounted = %+v, want 9002 then 9004", report.Unaccounted)
	}
	if report.Accounted != 0 || report.Absent != 0 {
		t.Fatalf("accounted = %d, absent = %d, want 0 and 0", report.Accounted, report.Absent)
	}
	if report.Drifted[0].ReservationID != "res-9001" || report.Drifted[1].ReservationID != "res-9005" {
		t.Fatalf("drift rows name %q and %q, want res-9001 then res-9005",
			report.Drifted[0].ReservationID, report.Drifted[1].ReservationID)
	}
}
