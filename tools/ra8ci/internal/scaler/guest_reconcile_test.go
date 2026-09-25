// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package scaler

import (
	"errors"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/proxmox"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

func reconcileGuest(vmid int, name, status string) proxmox.VM {
	return proxmox.VM{
		Identity: proxmox.Identity{VMID: vmid, Node: "pve", Pool: "ra8-tf-lab", Name: name},
		Status:   status,
	}
}

func reconcileReservation(id string, vmid int, name string) store.RunnerVM {
	return store.RunnerVM{
		ID:            id,
		RunnerVMInput: store.RunnerVMInput{VMID: vmid, Node: "pve", Pool: "ra8-tf-lab", Storage: "ra8-tf-lab", Name: name},
	}
}

// The case the whole pass exists for: an applied guest the ledger never
// recorded is named by no row, so nothing else in this package looks for it.
func TestGuestWithNoReservationIsReportedUnaccounted(t *testing.T) {
	report, err := ReconcileObservedGuests(
		[]proxmox.VM{
			reconcileGuest(9000, "ra8-lab-ci-9000", "running"),
			reconcileGuest(9001, "ra8-lab-ci-9001", "stopped"),
		},
		[]store.RunnerVM{reconcileReservation("res-9000", 9000, "ra8-lab-ci-9000")},
	)
	if err != nil {
		t.Fatalf("reconcile failed: %v", err)
	}
	if len(report.Unaccounted) != 1 || report.Unaccounted[0].Identity.VMID != 9001 {
		t.Fatalf("unaccounted = %+v, want exactly VMID 9001", report.Unaccounted)
	}
	if report.Unaccounted[0].Status != "stopped" {
		t.Fatalf("status = %q, want the observed status", report.Unaccounted[0].Status)
	}
	if report.Accounted != 1 || len(report.Drifted) != 0 || report.Absent != 0 {
		t.Fatalf("accounted=%d drifted=%d absent=%d, want 1/0/0", report.Accounted, len(report.Drifted), report.Absent)
	}
	if report.Clean() {
		t.Fatal("a report holding an unaccounted guest called itself clean")
	}
}

func TestMatchedGuestsAreAccountedAndTheReportIsClean(t *testing.T) {
	report, err := ReconcileObservedGuests(
		[]proxmox.VM{reconcileGuest(9000, "ra8-lab-ci-9000", "running")},
		[]store.RunnerVM{reconcileReservation("res-9000", 9000, "ra8-lab-ci-9000")},
	)
	if err != nil || report.Accounted != 1 || !report.Clean() {
		t.Fatalf("report = %+v, err = %v; want one accounted guest and a clean report", report, err)
	}
}

// Storage is set on the reservation and empty on everything Client.List
// returns, so comparing it would report every guest as drifted.
func TestFieldsAClusterListingDoesNotCarryAreNotComparedAsDrift(t *testing.T) {
	report, err := ReconcileObservedGuests(
		[]proxmox.VM{reconcileGuest(9000, "ra8-lab-ci-9000", "running")},
		[]store.RunnerVM{reconcileReservation("res-9000", 9000, "ra8-lab-ci-9000")},
	)
	if err != nil || len(report.Drifted) != 0 {
		t.Fatalf("drifted = %+v, err = %v; want no drift from an unlisted field", report.Drifted, err)
	}
}

func TestGuestDisagreeingWithItsReservationIsReportedAsDrift(t *testing.T) {
	tests := []struct {
		name    string
		observe func(*proxmox.VM)
	}{
		{name: "name", observe: func(v *proxmox.VM) { v.Identity.Name = "ra8-lab-ci-9099" }},
		{name: "node", observe: func(v *proxmox.VM) { v.Identity.Node = "pve2" }},
		{name: "pool", observe: func(v *proxmox.VM) { v.Identity.Pool = "other" }},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			guest := reconcileGuest(9000, "ra8-lab-ci-9000", "running")
			test.observe(&guest)
			report, err := ReconcileObservedGuests([]proxmox.VM{guest},
				[]store.RunnerVM{reconcileReservation("res-9000", 9000, "ra8-lab-ci-9000")})
			if err != nil {
				t.Fatalf("reconcile failed: %v", err)
			}
			if len(report.Drifted) != 1 || report.Drifted[0].ReservationID != "res-9000" {
				t.Fatalf("drifted = %+v, want one entry naming res-9000", report.Drifted)
			}
			if report.Accounted != 0 || len(report.Unaccounted) != 0 {
				t.Fatalf("drift counted as accounted=%d unaccounted=%d", report.Accounted, len(report.Unaccounted))
			}
		})
	}
}

// A reservation Proxmox does not report is not a leak: a row recorded before
// its clone, or after its destroy, is absent and correct.
func TestReservationWithNoGuestIsAbsentAndNotUnaccounted(t *testing.T) {
	report, err := ReconcileObservedGuests(nil,
		[]store.RunnerVM{reconcileReservation("res-9000", 9000, "ra8-lab-ci-9000")})
	if err != nil || report.Absent != 1 || len(report.Unaccounted) != 0 || !report.Clean() {
		t.Fatalf("report = %+v, err = %v; want one absent reservation and a clean report", report, err)
	}
}

func TestReservationNamingNoVMIDAccountsForNothing(t *testing.T) {
	report, err := ReconcileObservedGuests(
		[]proxmox.VM{reconcileGuest(9000, "ra8-lab-ci-9000", "running")},
		[]store.RunnerVM{reconcileReservation("res-pending", 0, "")},
	)
	if err != nil {
		t.Fatalf("reconcile failed: %v", err)
	}
	if report.Unassigned != 1 || len(report.Unaccounted) != 1 {
		t.Fatalf("unassigned=%d unaccounted=%d, want 1/1", report.Unassigned, len(report.Unaccounted))
	}
}

// Either ambiguity makes "the reservation for this guest" meaningless, and a
// confident answer over a set nobody can act on is worse than a refusal.
func TestAmbiguousInputIsRefusedRatherThanResolved(t *testing.T) {
	guest := reconcileGuest(9000, "ra8-lab-ci-9000", "running")
	if _, err := ReconcileObservedGuests([]proxmox.VM{guest, guest},
		[]store.RunnerVM{reconcileReservation("res-9000", 9000, "ra8-lab-ci-9000")}); !errors.Is(err, ErrGuestSetAmbiguous) {
		t.Fatalf("duplicate observation error = %v, want ErrGuestSetAmbiguous", err)
	}
	if _, err := ReconcileObservedGuests([]proxmox.VM{guest}, []store.RunnerVM{
		reconcileReservation("res-a", 9000, "ra8-lab-ci-9000"),
		reconcileReservation("res-b", 9000, "ra8-lab-ci-9000"),
	}); !errors.Is(err, ErrGuestSetAmbiguous) {
		t.Fatalf("duplicate claim error = %v, want ErrGuestSetAmbiguous", err)
	}
}

func TestReportsAreOrderedByVMID(t *testing.T) {
	report, err := ReconcileObservedGuests(
		[]proxmox.VM{
			reconcileGuest(9003, "ra8-lab-ci-9003", "running"),
			reconcileGuest(9001, "ra8-lab-ci-9001", "stopped"),
			reconcileGuest(9002, "ra8-lab-ci-9002", "running"),
		}, nil)
	if err != nil || len(report.Unaccounted) != 3 {
		t.Fatalf("report = %+v, err = %v", report, err)
	}
	for index, want := range []int{9001, 9002, 9003} {
		if report.Unaccounted[index].Identity.VMID != want {
			t.Fatalf("unaccounted[%d] = %d, want %d", index, report.Unaccounted[index].Identity.VMID, want)
		}
	}
}

// Nothing here destroys. The pass reports, and the decision stays with a human.
func TestEmptyInputIsCleanAndEmpty(t *testing.T) {
	report, err := ReconcileObservedGuests(nil, nil)
	if err != nil || !report.Clean() || report.Accounted != 0 || report.Absent != 0 || report.Unassigned != 0 {
		t.Fatalf("report = %+v, err = %v; want an empty clean report", report, err)
	}
}
