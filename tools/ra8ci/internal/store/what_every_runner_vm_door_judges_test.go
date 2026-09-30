// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"context"
	"errors"
	"strings"
	"testing"
	"time"

	"github.com/jackc/pgx/v5/pgxpool"
)

// What every runner-VM door judges before it reaches the database.
//
// These guards are the plane's only defence against a scaler bug becoming a
// locked row or a half-written reservation, and they are the arms an
// integration test never exercises: a test with a live database sends
// well-formed arguments, so the refusals stay unrun while everything behind
// them is covered.
//
// THE FIXTURE IS THE POINT. A Store with no pool refuses EVERYTHING as
// invalid, because the nil pool is itself one term of the same condition, so a
// test written against one would pass with every argument check deleted. So
// these run against a store whose pool is built but points nowhere: the pool
// is non-nil, the argument terms are the only thing that can answer invalid,
// and a well-formed call goes past them and fails reaching the database
// instead. That difference, invalid versus unavailable, is what makes each
// case below prove something.

func unreachablePlane(t *testing.T) *Store {
	t.Helper()
	pool, err := pgxpool.New(context.Background(),
		"postgres://ra8ci:ra8ci@127.0.0.1:1/ra8ci?connect_timeout=1")
	if err != nil {
		t.Fatalf("build a pool that points nowhere: %v", err)
	}
	t.Cleanup(pool.Close)
	return &Store{pool: pool}
}

// refusedBefore asserts the call never reached the database, and reached asserts
// it did. Neither reads the message: the sentinel is the contract callers match
// on, and ErrInvalid is the caller's to fix while ErrUnavailable is ours.
func refusedBefore(t *testing.T, what string, err error) {
	t.Helper()
	if !errors.Is(err, ErrInvalid) {
		t.Fatalf("%s: err %v, want ErrInvalid", what, err)
	}
}

func reached(t *testing.T, what string, err error) {
	t.Helper()
	if errors.Is(err, ErrInvalid) {
		t.Fatalf("%s: refused as invalid, so the arguments above prove nothing: %v", what, err)
	}
	if !errors.Is(err, ErrUnavailable) {
		t.Fatalf("%s: err %v, want ErrUnavailable from a plane that points nowhere", what, err)
	}
}

const (
	doorReservation = "01996f90-3415-7cfe-8ff1-600058131b10"
	doorOperation   = "01996f90-3415-7cfe-8ff1-600058131b11"
)

func TestEveryRunnerVMReadNamesWhatItIsReading(t *testing.T) {
	plane, ctx := unreachablePlane(t), context.Background()

	_, err := plane.GetRunnerVM(ctx, "not-an-identifier")
	refusedBefore(t, "a reservation that is not an identifier", err)
	_, err = plane.GetRunnerVM(ctx, "")
	refusedBefore(t, "no reservation", err)
	_, err = plane.GetRunnerVM(ctx, doorReservation)
	reached(t, "a stated reservation", err)

	_, err = plane.GetRunnerVMOperation(ctx, "not-an-identifier")
	refusedBefore(t, "an operation that is not an identifier", err)
	_, err = plane.GetRunnerVMOperation(ctx, doorOperation)
	reached(t, "a stated operation", err)

	// A job lookup is keyed on the pair, so both halves are judged.
	_, err = plane.GetRunnerVMByJob(ctx, 0, "job-1")
	refusedBefore(t, "no scale set", err)
	_, err = plane.GetRunnerVMByJob(ctx, -1, "job-1")
	refusedBefore(t, "a negative scale set", err)
	_, err = plane.GetRunnerVMByJob(ctx, 1, "")
	refusedBefore(t, "no job", err)
	_, err = plane.GetRunnerVMByJob(ctx, 1, strings.Repeat("j", 129))
	refusedBefore(t, "a job past its column", err)
	_, err = plane.GetRunnerVMByJob(ctx, 1, strings.Repeat("j", 128))
	reached(t, "a job at its bound", err)
}

// The two list doors carry a page size, and a page the database will not serve
// is refused here rather than turned into a query with a nonsense LIMIT.
func TestEveryRunnerVMListStatesItsPage(t *testing.T) {
	plane, ctx := unreachablePlane(t), context.Background()
	now := time.Date(2026, 9, 24, 12, 0, 0, 0, time.UTC)

	for _, c := range []struct {
		name  string
		set   int64
		when  time.Time
		limit int
	}{
		{"no scale set", 0, now, 10},
		{"no clock", 1, time.Time{}, 10},
		{"no page", 1, now, 0},
		{"a negative page", 1, now, -1},
		{"a page past the ceiling", 1, now, 1001},
	} {
		_, err := plane.ListExpiredUnclaimedRunnerVMs(ctx, c.set, c.when, c.limit)
		refusedBefore(t, c.name, err)
	}
	_, err := plane.ListExpiredUnclaimedRunnerVMs(ctx, 1, now, 1000)
	reached(t, "a page at the ceiling", err)

	for _, c := range []struct {
		name  string
		set   int64
		limit int
	}{
		{"no scale set", 0, 10},
		{"no page", 1, 0},
		{"a page past the ceiling", 1, 1001},
	} {
		_, err := plane.ListUnresolvedRunnerVMs(ctx, c.set, c.limit)
		refusedBefore(t, c.name, err)
	}
	_, err = plane.ListUnresolvedRunnerVMs(ctx, 1, 1)
	reached(t, "a page of one", err)
}

// Every write door names an actor and the generation it believes the VM is at.
// The generation is the fence: a scaler acting on a stale read must be refused
// rather than allowed to move a machine someone else already moved, and a
// generation below 1 is not a stale fence but an absent one.
func TestEveryRunnerVMWriteNamesAnActorAndAFence(t *testing.T) {
	plane, ctx := unreachablePlane(t), context.Background()
	safety := RunnerVMSafetyEvidence{}

	type door struct {
		name string
		call func(actor string, generation int64) error
	}
	doors := []door{
		{"begin an operation", func(actor string, generation int64) error {
			_, err := plane.BeginRunnerVMOperation(ctx, actor, doorReservation, generation, "start", safety)
			return err
		}},
		{"resolve an operation", func(actor string, generation int64) error {
			_, err := plane.ResolveRunnerVMOperation(ctx, actor, doorReservation, generation, doorOperation, RunnerVMResolution{})
			return err
		}},
		{"mark draining", func(actor string, generation int64) error {
			_, err := plane.MarkRunnerVMDraining(ctx, actor, doorReservation, generation)
			return err
		}},
		{"begin a terraform apply", func(actor string, generation int64) error {
			_, err := plane.BeginRunnerVMTerraformApply(ctx, actor, doorReservation, generation, doorOperation, strings.Repeat("a", 64))
			return err
		}},
		{"record a UPID", func(actor string, generation int64) error {
			return plane.RecordRunnerVMUPID(ctx, actor, doorReservation, generation, doorOperation, "UPID:lab-1:0000ABCD:qmstart")
		}},
	}

	for _, d := range doors {
		refusedBefore(t, d.name+" with no actor", d.call("", 1))
		refusedBefore(t, d.name+" with an actor past its column", d.call(strings.Repeat("s", 257), 1))
		refusedBefore(t, d.name+" with no fence", d.call("scaler", 0))
		refusedBefore(t, d.name+" with a negative fence", d.call("scaler", -1))
		reached(t, d.name+" as stated", d.call(strings.Repeat("s", 256), 1))
	}
}

// A claim is the one write with no fence: a single-use credential is redeemed
// once and the row's own state decides the rest, so only the actor and the
// reservation are judged here.
func TestAClaimNamesItsActorAndReservation(t *testing.T) {
	plane, ctx := unreachablePlane(t), context.Background()

	_, err := plane.MarkRunnerVMClaimed(ctx, "", doorReservation)
	refusedBefore(t, "no actor", err)
	_, err = plane.MarkRunnerVMClaimed(ctx, strings.Repeat("s", 257), doorReservation)
	refusedBefore(t, "an actor past its column", err)
	_, err = plane.MarkRunnerVMClaimed(ctx, "runner", "not-an-identifier")
	refusedBefore(t, "a reservation that is not an identifier", err)
	_, err = plane.MarkRunnerVMClaimed(ctx, "runner", doorReservation)
	reached(t, "a stated claim", err)
}

// A reservation is permanent, so the whole machine it describes is judged
// before a row exists. validRunnerVMInput is pinned field by field elsewhere;
// what this adds is that the door consults it at all, and that the actor and
// the unclaimed deadline are judged beside it.
func TestAReservationIsJudgedWholeAtTheDoor(t *testing.T) {
	plane, ctx := unreachablePlane(t), context.Background()
	deadline := time.Now().UTC().Add(10 * time.Minute)

	_, _, err := plane.ReserveRunnerVM(ctx, "", aRunnerVMInput(), deadline)
	refusedBefore(t, "no actor", err)
	_, _, err = plane.ReserveRunnerVM(ctx, strings.Repeat("s", 257), aRunnerVMInput(), deadline)
	refusedBefore(t, "an actor past its column", err)

	bent := aRunnerVMInput()
	bent.VMID = bent.TemplateVMID
	_, _, err = plane.ReserveRunnerVM(ctx, "scaler", bent, deadline)
	refusedBefore(t, "a VM that is its own template", err)

	// The deadline is judged after the arguments and answers on its own
	// terms, so an absent one is refused without being invalid input.
	if _, _, err := plane.ReserveRunnerVM(ctx, "scaler", aRunnerVMInput(), time.Time{}); err == nil {
		t.Fatal("a reservation with no unclaimed deadline was accepted")
	}
	_, _, err = plane.ReserveRunnerVM(ctx, "scaler", aRunnerVMInput(), deadline)
	reached(t, "a whole reservation", err)
}

// A plan is evidence about a tree, and every hash in it is judged before the
// row is touched: an unreadable digest here would be stored and later compared
// against a real one, which is a mismatch nobody can explain.
func TestATerraformPlanStatesEveryDigestItClaims(t *testing.T) {
	plane, ctx := unreachablePlane(t), context.Background()
	whole := func() RunnerVMTerraformPlanEvidence {
		return RunnerVMTerraformPlanEvidence{
			TerraformVersion:    "1.9.5",
			PlanSHA256:          strings.Repeat("a", 64),
			ModuleSHA256:        strings.Repeat("b", 64),
			InputSHA256:         strings.Repeat("c", 64),
			ProviderLockSHA256:  strings.Repeat("d", 64),
			StateIdentitySHA256: strings.Repeat("e", 64),
		}
	}
	record := func(evidence RunnerVMTerraformPlanEvidence) error {
		return plane.RecordRunnerVMTerraformPlan(ctx, "scaler", doorReservation, 1, doorOperation, evidence)
	}

	for name, bend := range map[string]func(*RunnerVMTerraformPlanEvidence){
		"no version":            func(e *RunnerVMTerraformPlanEvidence) { e.TerraformVersion = "" },
		"a version with a v":    func(e *RunnerVMTerraformPlanEvidence) { e.TerraformVersion = "v1.9.5" },
		"a two-part version":    func(e *RunnerVMTerraformPlanEvidence) { e.TerraformVersion = "1.9" },
		"no plan digest":        func(e *RunnerVMTerraformPlanEvidence) { e.PlanSHA256 = "" },
		"a short plan digest":   func(e *RunnerVMTerraformPlanEvidence) { e.PlanSHA256 = strings.Repeat("a", 63) },
		"an uppercase digest":   func(e *RunnerVMTerraformPlanEvidence) { e.PlanSHA256 = strings.Repeat("A", 64) },
		"no module digest":      func(e *RunnerVMTerraformPlanEvidence) { e.ModuleSHA256 = "" },
		"a commit-sized module": func(e *RunnerVMTerraformPlanEvidence) { e.ModuleSHA256 = strings.Repeat("b", 40) },
		"no input digest":       func(e *RunnerVMTerraformPlanEvidence) { e.InputSHA256 = "" },
		// The provider lock and the state identity are the two an
		// operator never types and so the two most likely to be left
		// off a synthesized plan. They are judged like the rest.
		"no provider lock":     func(e *RunnerVMTerraformPlanEvidence) { e.ProviderLockSHA256 = "" },
		"no state identity":    func(e *RunnerVMTerraformPlanEvidence) { e.StateIdentitySHA256 = "" },
		"a bad state identity": func(e *RunnerVMTerraformPlanEvidence) { e.StateIdentitySHA256 = "not-a-digest" },
	} {
		evidence := whole()
		bend(&evidence)
		refusedBefore(t, name, record(evidence))
	}
	reached(t, "a whole plan", record(whole()))

	// The apply that follows names the plan it is applying, and the same
	// digest shape is judged there.
	_, err := plane.BeginRunnerVMTerraformApply(ctx, "scaler", doorReservation, 1, doorOperation, "")
	refusedBefore(t, "an apply naming no plan", err)
	_, err = plane.BeginRunnerVMTerraformApply(ctx, "scaler", doorReservation, 1, doorOperation, strings.Repeat("A", 64))
	refusedBefore(t, "an apply naming an uppercase digest", err)
}

// A UPID is Proxmox's own task identifier and the plane stores it verbatim to
// hand back to Proxmox later. It is judged by shape because a malformed one is
// useless the moment anybody tries to follow it.
func TestAProxmoxTaskIdentifierIsJudgedByShape(t *testing.T) {
	plane, ctx := unreachablePlane(t), context.Background()
	record := func(upid string) error {
		return plane.RecordRunnerVMUPID(ctx, "scaler", doorReservation, 1, doorOperation, upid)
	}

	for _, c := range []struct{ name, upid string }{
		{"no identifier", ""},
		{"no prefix", "lab-1:0000ABCD:qmstart"},
		{"a lowercase prefix", "upid:lab-1:0000ABCD:qmstart"},
		{"one segment", "UPID:lab-1"},
		{"a space", "UPID:lab 1:0000ABCD:qmstart"},
		{"a newline", "UPID:lab-1:0000ABCD:qmstart\n"},
		{"past its column", "UPID:lab-1:" + strings.Repeat("a", 512)},
	} {
		refusedBefore(t, c.name, record(c.upid))
	}
	reached(t, "a stated task identifier", record("UPID:lab-1:0000ABCD:00001234:6F1A2B3C:qmstart:9000:root@pam!scaler:"))
}
