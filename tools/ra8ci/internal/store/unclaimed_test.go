// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"errors"
	"strings"
	"testing"
	"time"
)

func reservationAt(state string, deadline time.Time, claimed *time.Time) RunnerVM {
	return RunnerVM{State: state, UnclaimedDeadline: deadline, ClaimedAt: claimed}
}

func TestUnclaimedLeaseBounds(t *testing.T) {
	for _, tc := range []struct {
		lease time.Duration
		valid bool
	}{
		{0, false},
		{-time.Minute, false},
		{time.Second, false},
		{MinUnclaimedLease, true},
		{DefaultUnclaimedLease, true},
		{MaxUnclaimedLease, true},
		{MaxUnclaimedLease + time.Second, false},
	} {
		if got := ValidUnclaimedLease(tc.lease); got != tc.valid {
			t.Fatalf("lease %s: valid=%v want %v", tc.lease, got, tc.valid)
		}
	}
	if DefaultUnclaimedLease < MinUnclaimedLease || DefaultUnclaimedLease > MaxUnclaimedLease {
		t.Fatal("the default lease is outside the bounds it is checked against")
	}
}

func TestUnclaimedDeadlineIsComputedOnce(t *testing.T) {
	now := time.Date(2026, 9, 24, 12, 0, 0, 0, time.UTC)
	deadline, err := UnclaimedDeadline(now, DefaultUnclaimedLease)
	if err != nil {
		t.Fatal(err)
	}
	if !deadline.Equal(now.Add(DefaultUnclaimedLease)) {
		t.Fatalf("deadline %s is not now plus the lease", deadline)
	}
	if err := ValidUnclaimedDeadline(now, deadline); err != nil {
		t.Fatalf("a deadline this package computed failed its own check: %v", err)
	}
	if _, err := UnclaimedDeadline(time.Time{}, DefaultUnclaimedLease); !errors.Is(err, ErrInvalid) {
		t.Fatalf("zero clock accepted: %v", err)
	}
	if _, err := UnclaimedDeadline(now, time.Second); !errors.Is(err, ErrInvalid) {
		t.Fatalf("one second lease accepted: %v", err)
	}
	if _, err := UnclaimedDeadline(now, 48*time.Hour); !errors.Is(err, ErrInvalid) {
		t.Fatalf("two day lease accepted: %v", err)
	}
}

func TestValidUnclaimedDeadlineRefusesThePast(t *testing.T) {
	now := time.Date(2026, 9, 24, 12, 0, 0, 0, time.UTC)
	for name, deadline := range map[string]time.Time{
		"unset":    {},
		"past":     now.Add(-time.Hour),
		"now":      now,
		"too soon": now.Add(time.Second),
		"too far":  now.Add(MaxUnclaimedLease + time.Minute),
	} {
		if err := ValidUnclaimedDeadline(now, deadline); !errors.Is(err, ErrInvalid) {
			t.Fatalf("%s deadline accepted: %v", name, err)
		}
	}
	if err := ValidUnclaimedDeadline(time.Time{}, now.Add(time.Hour)); !errors.Is(err, ErrInvalid) {
		t.Fatal("zero clock accepted")
	}
}

func TestUnclaimedExpiredNeedsAllThreeConditions(t *testing.T) {
	now := time.Date(2026, 9, 24, 12, 0, 0, 0, time.UTC)
	past, future := now.Add(-time.Minute), now.Add(time.Minute)
	claimed := now.Add(-time.Hour)

	if !UnclaimedExpired(reservationAt("reserved", past, nil), now) {
		t.Fatal("an unclaimed reservation past its deadline is the whole point")
	}
	if !UnclaimedExpired(reservationAt("registered", now, nil), now) {
		t.Fatal("a deadline exactly now has passed")
	}
	if UnclaimedExpired(reservationAt("reserved", future, nil), now) {
		t.Fatal("a reservation inside its deadline was reaped")
	}
	if UnclaimedExpired(reservationAt("registered", past, &claimed), now) {
		t.Fatal("a claimed reservation stays out of reach once a job took it")
	}
	if UnclaimedExpired(reservationAt("released", past, nil), now) {
		t.Fatal("a released reservation has nothing left to revoke")
	}
	if UnclaimedExpired(reservationAt("reserved", time.Time{}, nil), now) {
		t.Fatal("a row with no deadline is a migration bug, not a candidate")
	}
}

// Every state a reservation can hold except released is reapable while
// unclaimed: the credential was minted before the guest existed, so a
// reservation that never got as far as cloning is exactly the case this
// reaper exists for.
func TestUnclaimedExpiredCoversEveryLiveState(t *testing.T) {
	now := time.Date(2026, 9, 24, 12, 0, 0, 0, time.UTC)
	past := now.Add(-time.Minute)
	states := []string{"reserved", "cloning", "stopped", "starting", "running",
		"registered", "draining", "stopping", "deleting", "released"}
	for _, state := range states {
		vm := reservationAt(state, past, nil)
		want := state != "released"
		if got := UnclaimedExpired(vm, now); got != want {
			t.Fatalf("state %q: expired=%v want %v", state, got, want)
		}
		if got := UnclaimedReservation(vm); got != want {
			t.Fatalf("state %q: unclaimed reservation=%v want %v", state, got, want)
		}
	}
}

// UnclaimedReservation is the clock-free half of UnclaimedExpired. A caller
// that listed a candidate and then locked the row re-checks it, so the two
// have to agree on everything except the deadline.
func TestUnclaimedReservationIsTheClockFreeHalf(t *testing.T) {
	now := time.Date(2026, 9, 24, 12, 0, 0, 0, time.UTC)
	past := now.Add(-time.Minute)
	claimed := now.Add(-time.Hour)
	for _, vm := range []RunnerVM{
		reservationAt("reserved", past, nil),
		reservationAt("released", past, nil),
		reservationAt("running", past, &claimed),
	} {
		if UnclaimedExpired(vm, now) && !UnclaimedReservation(vm) {
			t.Fatalf("%+v is expired but not an unclaimed reservation", vm)
		}
	}
}

// The candidate query is built from one format string so the SQL and the Go
// predicate cannot drift. Pin the three conditions and the placeholder.
func TestUnclaimedCandidatePredicate(t *testing.T) {
	clause := strings.ToLower(unclaimedCandidate)
	for _, want := range []string{"claimed_at is null", "state <> 'released'", "unclaimed_deadline <= $%d"} {
		if !strings.Contains(clause, want) {
			t.Fatalf("candidate predicate %q lost %q", unclaimedCandidate, want)
		}
	}
}

// The reservation carries the deadline, so the column list has to fetch it:
// a scan that silently left UnclaimedDeadline zero would make every row look
// like a migration bug to UnclaimedExpired.
func TestRunnerVMColumnsCarryTheDeadline(t *testing.T) {
	for _, column := range []string{"unclaimed_deadline", "claimed_at"} {
		if !strings.Contains(runnerVMColumns, column) {
			t.Fatalf("runnerVMColumns does not select %q", column)
		}
	}
}
