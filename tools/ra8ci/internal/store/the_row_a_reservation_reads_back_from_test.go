// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"database/sql"
	"errors"
	"testing"
	"time"
)

// A reservation row is read back through one scan, and six of its columns are
// nullable while the fields they land in are not. What that scan does with a
// NULL is what a caller later reads as "no runner registered yet" or "never
// claimed", so it is worth pinning without a database behind it.

// plannedRow is a pgx.Row that hands back one prepared row. Scan writes each
// value through the destination pointer the production code passed, so the
// column order under test is the real one rather than a restatement of it.
type plannedRow struct {
	values []any
	err    error
}

func (r plannedRow) Scan(dest ...any) error {
	if r.err != nil {
		return r.err
	}
	if len(dest) != len(r.values) {
		return errors.New("row width does not match the scan")
	}
	for i, target := range dest {
		value := r.values[i]
		switch into := target.(type) {
		case *string:
			*into = value.(string)
		case *int:
			*into = value.(int)
		case *int64:
			*into = value.(int64)
		case *bool:
			*into = value.(bool)
		case *time.Time:
			*into = value.(time.Time)
		case *sql.NullString:
			*into = value.(sql.NullString)
		case *sql.NullInt64:
			*into = value.(sql.NullInt64)
		case *sql.NullTime:
			*into = value.(sql.NullTime)
		default:
			return errors.New("a reservation column was scanned into an unexpected kind")
		}
	}
	return nil
}

func reservationColumns(digest, operation, runnerName sql.NullString, runnerID sql.NullInt64,
	claimed, ended sql.NullTime) []any {
	moment := time.Date(2026, 9, 29, 21, 0, 0, 0, time.UTC)
	return []any{
		"01996f90-3415-7cfe-8ff1-600058131aff", // id
		int64(7),                               // scale_set_id
		"job-1",                                // job_id
		int64(11),                              // runner_request_id
		int64(12),                              // workflow_run_id
		3,                                      // workflow_attempt
		"bsikar/ra8-firmware",                  // repository
		"refs/heads/ra8ci/dev",                 // workflow_ref
		"0123456789abcdef0123456789abcdef01234567", // commit_sha
		9001,                                   // vmid
		"pve-1",                                // node
		"lab",                                  // pool
		"local-zfs",                            // storage
		"ra8-lab-runner-1",                     // vm_name
		104,                                    // template_vmid
		"ra8-lab-template",                     // template_name
		digest,                                 // template_digest
		"01996f90-3415-7cfe-8ff1-600058131afe", // creation_operation_id
		"reserved",                             // state
		int64(4),                               // generation
		false,                                  // unknown_outcome
		false,                                  // cleanup_requested
		operation,                              // current_operation_id
		runnerID,                               // external_runner_id
		runnerName,                             // external_runner_name
		moment.Add(30 * time.Minute),           // unclaimed_deadline
		claimed,                                // claimed_at
		moment,                                 // created_at
		moment,                                 // updated_at
		ended,                                  // ended_at
	}
}

func someText(text string) sql.NullString { return sql.NullString{String: text, Valid: true} }

// A reservation that has been through its whole life reads back whole: every
// nullable column present, and the two that are pointers carrying times.
func TestAWholeReservationReadsBackWhole(t *testing.T) {
	claimedAt := time.Date(2026, 9, 29, 21, 5, 0, 0, time.UTC)
	endedAt := time.Date(2026, 9, 29, 21, 40, 0, 0, time.UTC)
	vm, err := scanRunnerVM(plannedRow{values: reservationColumns(
		someText("0123456789abcdef0123456789abcdef01234567"),
		someText("01996f90-3415-7cfe-8ff1-600058131afd"),
		someText("runner-1"), sql.NullInt64{Int64: 55, Valid: true},
		sql.NullTime{Time: claimedAt, Valid: true}, sql.NullTime{Time: endedAt, Valid: true})})
	if err != nil {
		t.Fatalf("a whole reservation row was refused: %v", err)
	}
	if vm.ID != "01996f90-3415-7cfe-8ff1-600058131aff" || vm.ScaleSetID != 7 || vm.JobID != "job-1" ||
		vm.VMID != 9001 || vm.Name != "ra8-lab-runner-1" || vm.State != "reserved" || vm.Generation != 4 {
		t.Fatalf("a reservation read back as %+v", vm)
	}
	if vm.CurrentOperationID != "01996f90-3415-7cfe-8ff1-600058131afd" ||
		vm.ExternalRunnerID != 55 || vm.ExternalRunnerName != "runner-1" {
		t.Fatalf("the runner identity read back as %q/%d/%q",
			vm.CurrentOperationID, vm.ExternalRunnerID, vm.ExternalRunnerName)
	}
	if vm.ClaimedAt == nil || !vm.ClaimedAt.Equal(claimedAt) {
		t.Fatalf("claimed_at read back as %v", vm.ClaimedAt)
	}
	if vm.EndedAt == nil || !vm.EndedAt.Equal(endedAt) {
		t.Fatalf("ended_at read back as %v", vm.EndedAt)
	}
}

// A reservation nobody has claimed yet has every nullable column empty, and
// the difference that matters is the shape: the string and integer columns
// collapse to their zero values, while the two time columns stay nil rather
// than becoming the year 1. A caller asks "has this been claimed" by testing
// the pointer, so a zero time in its place would read as claimed in 1 AD.
func TestAnUnclaimedReservationKeepsItsAbsencesAbsent(t *testing.T) {
	vm, err := scanRunnerVM(plannedRow{values: reservationColumns(
		sql.NullString{}, sql.NullString{}, sql.NullString{}, sql.NullInt64{},
		sql.NullTime{}, sql.NullTime{})})
	if err != nil {
		t.Fatalf("an unclaimed reservation row was refused: %v", err)
	}
	if vm.TemplateDigest != "" || vm.CurrentOperationID != "" ||
		vm.ExternalRunnerName != "" || vm.ExternalRunnerID != 0 {
		t.Fatalf("absent columns read back as %q/%q/%q/%d", vm.TemplateDigest,
			vm.CurrentOperationID, vm.ExternalRunnerName, vm.ExternalRunnerID)
	}
	if vm.ClaimedAt != nil {
		t.Fatalf("an unclaimed reservation read back a claim at %v", *vm.ClaimedAt)
	}
	if vm.EndedAt != nil {
		t.Fatalf("a live reservation read back an end at %v", *vm.EndedAt)
	}
	// The columns that are never null still arrive, so the absences above
	// are the nullable ones rather than a row that failed to read at all.
	if vm.ID == "" || vm.UnclaimedDeadline.IsZero() || vm.CreatedAt.IsZero() {
		t.Fatalf("a row with null decorations lost its identity: %+v", vm)
	}
}

// A machine that has been claimed but is still running is the state a caller
// most often has to tell apart, so the two time pointers are pinned moving
// independently rather than together.
func TestAClaimedButLiveReservationCarriesOnlyItsClaim(t *testing.T) {
	claimedAt := time.Date(2026, 9, 29, 21, 5, 0, 0, time.UTC)
	vm, err := scanRunnerVM(plannedRow{values: reservationColumns(
		sql.NullString{}, sql.NullString{}, sql.NullString{}, sql.NullInt64{},
		sql.NullTime{Time: claimedAt, Valid: true}, sql.NullTime{})})
	if err != nil {
		t.Fatalf("a claimed live reservation was refused: %v", err)
	}
	if vm.ClaimedAt == nil || !vm.ClaimedAt.Equal(claimedAt) || vm.EndedAt != nil {
		t.Fatalf("a claimed live reservation read back claimed %v ended %v", vm.ClaimedAt, vm.EndedAt)
	}
}

// A scan that fails hands the error back rather than swallowing it, and the
// partially-filled row that comes with it is never to be read as a machine.
func TestARefusedScanIsHandedBack(t *testing.T) {
	refusal := errors.New("no rows in result set")
	vm, err := scanRunnerVM(plannedRow{err: refusal})
	if !errors.Is(err, refusal) {
		t.Fatalf("a refused scan answered %v", err)
	}
	if vm.ID != "" || vm.State != "" {
		t.Fatalf("a refused scan still produced %+v", vm)
	}
}

// The destroy step of the unclaimed sequence asks whether a guest was ever
// cloned. Only the two states where no machine can exist answer no; every
// other state, including a clone whose outcome nobody heard, answers yes,
// because that is exactly the guest a reaper must not walk past.
func TestOnlyAReservationWithNoMachineIsWalkedPast(t *testing.T) {
	for _, state := range []string{"reserved", "released"} {
		if UnclaimedGuestExists(RunnerVM{State: state}) {
			t.Fatalf("a %q reservation was sent to the destroy step", state)
		}
	}
	for _, state := range []string{"cloning", "stopped", "starting", "running",
		"draining", "stopping", "deleting"} {
		if !UnclaimedGuestExists(RunnerVM{State: state}) {
			t.Fatalf("a %q reservation was walked past with a guest still on the hypervisor", state)
		}
	}
	// An unstated or unrecognised state is generous in the same direction:
	// the reaper asks the hypervisor rather than assuming nothing is there.
	for _, state := range []string{"", "RESERVED", "reserved ", "unknown"} {
		if !UnclaimedGuestExists(RunnerVM{State: state}) {
			t.Fatalf("a reservation in state %q was assumed to have no machine", state)
		}
	}
}
