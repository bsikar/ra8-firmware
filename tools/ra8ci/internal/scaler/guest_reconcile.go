// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package scaler

import (
	"errors"
	"fmt"
	"sort"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/proxmox"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// Comparing what Proxmox actually holds against what the ledger accounts for.
//
// Every cleanup path in this package starts from a durable row: the reaper
// takes its queue from ListExpiredUnclaimedRunnerVMs, and all four undo steps
// key off a store.RunnerVM. That is the right shape for a reservation this
// plane created and recorded, and it has one blind spot. A guest that
// Terraform applied but that never got a durable row, the UnknownOutcomeError
// path where the apply committed and the ledger write did not, is named by no
// row, so nothing ever looks for it. It sits on a reserved VMID, holding the
// address and the bridge a later reservation is entitled to, and the next
// clone onto that VMID is the first thing that notices.
//
// This pass REPORTS, it does not destroy. Destroying a guest the ledger
// cannot explain is exactly the operation that most deserves a human, and the
// reporting half is what makes that judgement possible at all.

// UnaccountedGuest is a guest Proxmox reports at a reserved VMID that no
// reservation in the compared set names.
type UnaccountedGuest struct {
	Identity proxmox.Identity
	Status   string
}

// GuestIdentityDrift is a guest whose observed identity disagrees with the
// reservation that claims its VMID.
type GuestIdentityDrift struct {
	VMID          int
	ReservationID string
	Observed      proxmox.Identity
	Recorded      proxmox.Identity
}

// GuestReconciliation is one comparison. Counts and lists describe the inputs
// as given: this type states what was seen, never what should be done about it.
type GuestReconciliation struct {
	// Unaccounted is every observed guest no reservation names, by VMID.
	Unaccounted []UnaccountedGuest
	// Drifted is every observed guest whose identity disagrees with the
	// reservation claiming its VMID, by VMID.
	Drifted []GuestIdentityDrift
	// Accounted is how many observed guests matched a reservation exactly.
	Accounted int
	// Unassigned is how many reservations named no VMID. Such a row
	// accounts for no guest, and saying so is more useful than dropping it.
	Unassigned int
	// Absent is how many reservations named a VMID Proxmox did not report.
	// This is NOT a leak and often not a fault: a reservation recorded
	// before its clone, or after its destroy, is absent and correct.
	Absent int
}

// ErrGuestSetAmbiguous is an input the comparison cannot interpret: the same
// VMID observed twice, or claimed by two reservations. Either makes "the
// reservation for this guest" meaningless, and picking a winner would report
// a confident answer about a set nobody can act on.
var ErrGuestSetAmbiguous = errors.New("guest reconciliation input is ambiguous")

// ErrGuestSetUnusable is an observed record that names no guest anything can
// address. A reservation naming no VMID is ordinary and is counted as
// Unassigned above, because a row exists before its clone and after its
// destroy. A guest is not the same case: Proxmox gives every guest a VMID, so
// a record without one is not a guest caught early, it is a record no path in
// this package can act on. Reporting it as unaccounted would send an operator
// hunting a leak at a VMID that cannot exist, and dropping it quietly would
// hide a listing this pass could not read.
var ErrGuestSetUnusable = errors.New("observed guest names no usable VMID")

// ReconcileObservedGuests compares the guests Proxmox reports against the
// reservations the ledger holds.
//
// It compares only VMID, node, pool and name, because those are the fields
// Client.List populates from /cluster/resources. Storage, the reservation
// marker and the configuration digest are not in that response, so a match
// here means "the same guest as far as the cluster listing can tell" and not
// "this guest is the one this reservation created". The deeper check is
// Client.Get's, which re-reads the marker and the disk storage per guest.
//
// The caller chooses which ledger read defines the accounting set, and that
// choice is the whole meaning of the result. A read narrower than "every
// reservation that could own a guest right now" reports live guests as
// unaccounted.
func ReconcileObservedGuests(observed []proxmox.VM, reservations []store.RunnerVM) (GuestReconciliation, error) {
	claims := make(map[int]store.RunnerVM, len(reservations))
	report := GuestReconciliation{}
	for _, vm := range reservations {
		if vm.VMID <= 0 {
			report.Unassigned++
			continue
		}
		if previous, duplicate := claims[vm.VMID]; duplicate {
			return GuestReconciliation{}, fmt.Errorf("%w: reservations %s and %s both claim VMID %d",
				ErrGuestSetAmbiguous, previous.ID, vm.ID, vm.VMID)
		}
		claims[vm.VMID] = vm
	}
	seen := make(map[int]struct{}, len(observed))
	for _, guest := range observed {
		if guest.Identity.VMID <= 0 {
			return GuestReconciliation{}, fmt.Errorf("%w: observed guest %q on node %q reported VMID %d",
				ErrGuestSetUnusable, guest.Identity.Name, guest.Identity.Node, guest.Identity.VMID)
		}
		if _, duplicate := seen[guest.Identity.VMID]; duplicate {
			return GuestReconciliation{}, fmt.Errorf("%w: VMID %d observed twice",
				ErrGuestSetAmbiguous, guest.Identity.VMID)
		}
		seen[guest.Identity.VMID] = struct{}{}
		claim, claimed := claims[guest.Identity.VMID]
		if !claimed {
			report.Unaccounted = append(report.Unaccounted,
				UnaccountedGuest{Identity: guest.Identity, Status: guest.Status})
			continue
		}
		recorded := proxmox.Identity{VMID: claim.VMID, Node: claim.Node, Pool: claim.Pool, Name: claim.Name}
		if !listedIdentityMatches(guest.Identity, recorded) {
			report.Drifted = append(report.Drifted, GuestIdentityDrift{
				VMID: guest.Identity.VMID, ReservationID: claim.ID,
				Observed: guest.Identity, Recorded: recorded,
			})
			continue
		}
		report.Accounted++
	}
	for vmid := range claims {
		if _, present := seen[vmid]; !present {
			report.Absent++
		}
	}
	sort.Slice(report.Unaccounted, func(a, b int) bool {
		return report.Unaccounted[a].Identity.VMID < report.Unaccounted[b].Identity.VMID
	})
	sort.Slice(report.Drifted, func(a, b int) bool { return report.Drifted[a].VMID < report.Drifted[b].VMID })
	return report, nil
}

// listedIdentityMatches compares only the fields a cluster listing carries.
// Storage, ReservationID and CreationOperationID are deliberately not compared:
// Client.List leaves them empty, so comparing them would report every guest as
// drifted and say nothing true.
func listedIdentityMatches(observed, recorded proxmox.Identity) bool {
	return observed.VMID == recorded.VMID && observed.Node == recorded.Node &&
		observed.Pool == recorded.Pool && observed.Name == recorded.Name
}

// Clean reports whether the comparison found nothing needing a human.
func (r GuestReconciliation) Clean() bool {
	return len(r.Unaccounted) == 0 && len(r.Drifted) == 0
}
