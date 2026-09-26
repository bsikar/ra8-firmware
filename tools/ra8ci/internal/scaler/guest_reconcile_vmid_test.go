// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package scaler

import (
	"errors"
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/proxmox"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// A guest always has a VMID. A record without one cannot be matched to a
// reservation, cannot be observed again, and cannot be destroyed, so calling
// it an unaccounted guest would name a leak at an address nothing can reach.
func TestObservedGuestWithNoVMIDIsRefused(t *testing.T) {
	report, err := ReconcileObservedGuests(
		[]proxmox.VM{reconcileGuest(0, "ra8-lab-ci-none", "running")},
		[]store.RunnerVM{reconcileReservation("res-9000", 9000, "ra8-lab-ci-9000")},
	)
	if !errors.Is(err, ErrGuestSetUnusable) {
		t.Fatalf("error = %v, want ErrGuestSetUnusable", err)
	}
	if len(report.Unaccounted) != 0 || report.Accounted != 0 || report.Absent != 0 {
		t.Fatalf("report = %+v, want the zero report on a refusal", report)
	}
}

func TestObservedGuestWithNegativeVMIDIsRefused(t *testing.T) {
	if _, err := ReconcileObservedGuests(
		[]proxmox.VM{reconcileGuest(-9000, "ra8-lab-ci-9000", "running")}, nil,
	); !errors.Is(err, ErrGuestSetUnusable) {
		t.Fatalf("error = %v, want ErrGuestSetUnusable", err)
	}
}

// The refusal has to name the record, because a VMID of zero is no help in
// finding which line of the listing was unreadable.
func TestTheRefusalNamesTheUnusableRecord(t *testing.T) {
	_, err := ReconcileObservedGuests(
		[]proxmox.VM{reconcileGuest(0, "ra8-lab-ci-orphan", "stopped")}, nil,
	)
	if err == nil || !strings.Contains(err.Error(), "ra8-lab-ci-orphan") || !strings.Contains(err.Error(), "pve") {
		t.Fatalf("error = %v, want the guest name and node in the message", err)
	}
}

// Two unusable records are refused as unusable, not collapsed into a
// duplicate-VMID ambiguity that blames the wrong thing.
func TestTwoUnusableRecordsAreNotReportedAsDuplicates(t *testing.T) {
	_, err := ReconcileObservedGuests(
		[]proxmox.VM{reconcileGuest(0, "ra8-lab-ci-a", "running"), reconcileGuest(0, "ra8-lab-ci-b", "running")},
		nil,
	)
	if !errors.Is(err, ErrGuestSetUnusable) || errors.Is(err, ErrGuestSetAmbiguous) {
		t.Fatalf("error = %v, want ErrGuestSetUnusable and not ErrGuestSetAmbiguous", err)
	}
}

// The asymmetry is deliberate and stays pinned: a row that names no VMID is
// an ordinary ledger state and is counted, not refused.
func TestReservationNamingNoVMIDIsStillCountedNotRefused(t *testing.T) {
	report, err := ReconcileObservedGuests(
		[]proxmox.VM{reconcileGuest(9000, "ra8-lab-ci-9000", "running")},
		[]store.RunnerVM{reconcileReservation("res-pending", 0, ""), reconcileReservation("res-9000", 9000, "ra8-lab-ci-9000")},
	)
	if err != nil {
		t.Fatalf("reconcile failed: %v", err)
	}
	if report.Unassigned != 1 || report.Accounted != 1 {
		t.Fatalf("unassigned=%d accounted=%d, want 1/1", report.Unassigned, report.Accounted)
	}
}

// A usable set still reconciles exactly as before.
func TestUsableGuestsAreUnaffectedByTheVMIDRefusal(t *testing.T) {
	report, err := ReconcileObservedGuests(
		[]proxmox.VM{reconcileGuest(9001, "ra8-lab-ci-9001", "running"), reconcileGuest(9000, "ra8-lab-ci-9000", "stopped")},
		[]store.RunnerVM{reconcileReservation("res-9000", 9000, "ra8-lab-ci-9000")},
	)
	if err != nil {
		t.Fatalf("reconcile failed: %v", err)
	}
	if report.Accounted != 1 || len(report.Unaccounted) != 1 || report.Unaccounted[0].Identity.VMID != 9001 {
		t.Fatalf("report = %+v, want 9000 accounted and 9001 unaccounted", report)
	}
}
