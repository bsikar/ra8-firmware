// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package proxmox

import "fmt"

// A listed guest's status is a fact a later pass acts on.
//
// List reports allowlisted guests so a sweep can find what the ledger cannot
// explain: scaler.ReconcileObservedGuests copies the status straight onto an
// UnaccountedGuest, and that line is what an operator reads before deciding
// whether a leaked reservation is idle or still carrying a job. Every other
// field in that report is held to the reservation before it is reported, and
// the status was not held to anything at all.
//
// This client acts on exactly two statuses. inspect refuses any other on the
// reservation path, because start, stop and destroy are the only operations it
// has and none of them has an answer for a guest that is paused, suspended, or
// reported as unknown by a node that has stopped answering. A listing is the
// same client reading the same guest, so it gives the same answer here rather
// than passing a status forward that no path in this package can act on.
//
// An unstated status is left alone, matching inspect, which compares the
// cluster record with status/current only when the record states one. A
// cluster listing that omits the field says nothing, and reporting that it
// said nothing is honest; a listing that states "unknown" is making a claim.
func checkListedStatus(r resource) error {
	if r.Status == "" || r.Status == "running" || r.Status == "stopped" {
		return nil
	}
	return fmt.Errorf("%w: allowed VM ID %d reports status %q, which this client cannot act on", ErrProtocol, r.VMID, r.Status)
}
