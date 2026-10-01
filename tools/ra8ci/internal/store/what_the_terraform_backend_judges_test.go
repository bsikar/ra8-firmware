// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"context"
	"crypto/aes"
	"crypto/cipher"
	"crypto/rand"
	"errors"
	"strings"
	"testing"

	"github.com/jackc/pgx/v5/pgxpool"
)

// What the Terraform state backend judges before the database, the six method
// guards left after the crypto round trip was pinned.
//
// This backend is reached by Terraform itself over HTTP, not by our own code,
// so its arguments arrive from a client we do not control and its refusals are
// the only thing standing between a malformed request and a state row. That
// makes the argument checks worth pinning on their own terms.
//
// Fixture as in #2717: the pool is built but points nowhere, so an argument
// term is the only thing that can answer invalid and a well-formed call goes
// past it and fails reaching the database instead. Two of these doors also
// consult the state key, so there is a sealing variant of the same plane.

func unreachableSealingPlane(t *testing.T) *Store {
	t.Helper()
	plane := unreachablePlane(t)
	key := make([]byte, 32)
	if _, err := rand.Read(key); err != nil {
		t.Fatal(err)
	}
	block, err := aes.NewCipher(key)
	if err != nil {
		t.Fatal(err)
	}
	aead, err := cipher.NewGCM(block)
	if err != nil {
		t.Fatal(err)
	}
	plane.terraformStateAEAD = aead
	return plane
}

// lockJSON is the existing helper in the_keys_a_terraform_state_is_held_under_test.go.
func aStateLock() TerraformStateLock {
	return TerraformStateLock{ID: doorOperation, Operation: "OperationTypeApply",
		Who: "terraform@lab-1", Version: "1.9.5", Path: "runner-vm/bench-one"}
}

// Every read names the reservation it is reading, and nothing else about the
// caller is taken on trust: the repository the backend authorizes against is
// looked up from the reservation rather than read out of the request.
func TestEveryTerraformStateReadNamesItsReservation(t *testing.T) {
	sealing, ctx := unreachableSealingPlane(t), context.Background()

	for _, bad := range []string{"", "not-an-identifier", "runner-vm/bench-one"} {
		_, _, err := sealing.ReadRunnerVMTerraformState(ctx, bad)
		refusedBefore(t, "a read of "+bad, err)
		_, err = sealing.RunnerVMTerraformStateLocked(ctx, bad)
		refusedBefore(t, "a lock check of "+bad, err)
		_, err = sealing.LookupRunnerVMRepository(ctx, bad)
		refusedBefore(t, "a repository lookup of "+bad, err)
	}

	_, _, err := sealing.ReadRunnerVMTerraformState(ctx, doorReservation)
	reached(t, "a stated read", err)
	_, err = sealing.RunnerVMTerraformStateLocked(ctx, doorReservation)
	reached(t, "a stated lock check", err)
	_, err = sealing.LookupRunnerVMRepository(ctx, doorReservation)
	reached(t, "a stated repository lookup", err)
}

// The read consults the state key, and the order matters: a malformed
// reservation is the caller's fault and answers invalid even on a plane with
// no key configured, while a well-formed one on that plane answers
// unavailable, because an unconfigured server is ours to fix and the client
// should not be told its request was wrong.
func TestAStateReadJudgesItsArgumentAheadOfOurConfiguration(t *testing.T) {
	unconfigured, ctx := unreachablePlane(t), context.Background()

	if _, err := unconfigured.stateAEAD(); !errors.Is(err, ErrUnavailable) {
		t.Fatalf("a plane with no state key: err %v, want ErrUnavailable", err)
	}
	_, _, err := unconfigured.ReadRunnerVMTerraformState(ctx, "not-an-identifier")
	refusedBefore(t, "a malformed read against an unconfigured plane", err)
	_, _, err = unconfigured.ReadRunnerVMTerraformState(ctx, doorReservation)
	if !errors.Is(err, ErrUnavailable) || errors.Is(err, ErrInvalid) {
		t.Fatalf("a stated read against an unconfigured plane: err %v, want ErrUnavailable alone", err)
	}

	// The lock check and the repository lookup never touch the key, so an
	// unconfigured plane is no obstacle to either.
	_, err = unconfigured.RunnerVMTerraformStateLocked(ctx, doorReservation)
	reached(t, "a lock check needs no state key", err)
	_, err = unconfigured.LookupRunnerVMRepository(ctx, doorReservation)
	reached(t, "a repository lookup needs no state key", err)
}

// Locking and unlocking both name an actor and a reservation, and then the
// lock info itself is parsed. Terraform sends that info as a JSON body, so it
// is the least trustworthy input the plane takes.
func TestALockRequestNamesAnActorAReservationAndItself(t *testing.T) {
	plane, ctx := unreachablePlane(t), context.Background()
	whole := lockJSON(t, aStateLock())

	type door struct {
		name string
		call func(actor, reservation string, raw []byte) error
	}
	for _, d := range []door{
		{"lock", func(actor, reservation string, raw []byte) error {
			_, _, err := plane.LockRunnerVMTerraformState(ctx, actor, reservation, raw)
			return err
		}},
		{"unlock", func(actor, reservation string, raw []byte) error {
			_, _, err := plane.UnlockRunnerVMTerraformState(ctx, actor, reservation, raw)
			return err
		}},
	} {
		refusedBefore(t, d.name+" with no actor", d.call("", doorReservation, whole))
		refusedBefore(t, d.name+" with no reservation", d.call("terraform", "", whole))
		refusedBefore(t, d.name+" of a reservation that is a path",
			d.call("terraform", "runner-vm/bench-one", whole))

		// The body is parsed after the arguments, and every way it can
		// be wrong is refused rather than stored as a lock nobody can
		// release.
		refusedBefore(t, d.name+" with no body", d.call("terraform", doorReservation, nil))
		refusedBefore(t, d.name+" with an empty body", d.call("terraform", doorReservation, []byte{}))
		refusedBefore(t, d.name+" with a body that is not JSON",
			d.call("terraform", doorReservation, []byte("locked")))
		refusedBefore(t, d.name+" with a body past its ceiling",
			d.call("terraform", doorReservation, make([]byte, 32<<10+1)))

		for name, bend := range map[string]func(*TerraformStateLock){
			"a lock with no ID":            func(l *TerraformStateLock) { l.ID = "" },
			"a lock whose ID is a name":    func(l *TerraformStateLock) { l.ID = "lock-1" },
			"a lock with no operation":     func(l *TerraformStateLock) { l.Operation = "" },
			"an operation past its column": func(l *TerraformStateLock) { l.Operation = strings.Repeat("o", 129) },
			"a holder past its column":     func(l *TerraformStateLock) { l.Who = strings.Repeat("w", 257) },
			"a version past its column":    func(l *TerraformStateLock) { l.Version = strings.Repeat("v", 65) },
			"a path past its column":       func(l *TerraformStateLock) { l.Path = strings.Repeat("p", 1025) },
		} {
			lock := aStateLock()
			bend(&lock)
			refusedBefore(t, d.name+" with "+name, d.call("terraform", doorReservation, lockJSON(t, lock)))
		}

		reached(t, "a stated "+d.name, d.call("terraform", doorReservation, whole))
	}
}

// A state write names the lock it is writing under. That is the whole safety
// property of the backend: a write with no lock ID, or one that is not an
// identifier, must never reach the row, because the row is what a later apply
// reads as the truth about a live machine.
func TestAStateWriteNamesTheLockItIsWritingUnder(t *testing.T) {
	sealing, ctx := unreachableSealingPlane(t), context.Background()

	for _, c := range []struct{ name, actor, reservation, lockID string }{
		{"no actor", "", doorReservation, doorOperation},
		{"no reservation", "terraform", "", doorOperation},
		{"a reservation that is a path", "terraform", "runner-vm/bench-one", doorOperation},
		{"no lock", "terraform", doorReservation, ""},
		{"a lock that is a name", "terraform", doorReservation, "lock-1"},
	} {
		refusedBefore(t, "a removal with "+c.name,
			sealing.updateRunnerVMTerraformState(ctx, c.actor, c.reservation, c.lockID, nil, true))
		refusedBefore(t, "a write with "+c.name,
			sealing.updateRunnerVMTerraformState(ctx, c.actor, c.reservation, c.lockID, []byte(`{"version":4}`), false))
	}

	// A removal carries no body to parse, so it reaches the database on
	// the arguments alone; that is what makes the refusals above the
	// argument check and not the parser.
	reached(t, "a stated removal",
		sealing.updateRunnerVMTerraformState(ctx, "terraform", doorReservation, doorOperation, nil, true))
}

// The pool itself is one term of every guard above, so a plane with no pool
// answers invalid rather than unavailable at these doors. That is worth
// pinning because the offline-sync doors deliberately do the opposite, and
// the difference is easy to lose in a later edit.
func TestAPlaneWithNoStoreIsInvalidAtTheseDoors(t *testing.T) {
	half, ctx := &Store{}, context.Background()

	_, _, err := half.ReadRunnerVMTerraformState(ctx, doorReservation)
	refusedBefore(t, "a read against a plane with no store", err)
	_, err = half.LookupRunnerVMRepository(ctx, doorReservation)
	refusedBefore(t, "a repository lookup against a plane with no store", err)

	var built *pgxpool.Pool
	if half.pool != built {
		t.Fatal("the fixture is not the half-built plane this case is about")
	}
}
