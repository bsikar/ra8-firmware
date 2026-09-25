package store

import (
	"errors"
	"testing"
	"time"
)

var sweepEpoch = time.Date(2026, 9, 24, 12, 0, 0, 0, time.UTC)

func TestExpiredLeaseArgsRefusesAReadWithNoClock(t *testing.T) {
	if _, err := expiredLeaseArgs(time.Time{}, 10); !errors.Is(err, ErrInvalid) {
		t.Fatalf("zero clock: got %v, want ErrInvalid", err)
	}
}

func TestExpiredLeaseArgsTakesTheDefaultPageForZero(t *testing.T) {
	args, err := expiredLeaseArgs(sweepEpoch, 0)
	if err != nil {
		t.Fatalf("args: %v", err)
	}
	if len(args) != 2 {
		t.Fatalf("args: got %d, want 2", len(args))
	}
	if args[1] != expiredLeasePage {
		t.Fatalf("page: got %v, want %d", args[1], expiredLeasePage)
	}
	if got := args[0].(time.Time); !got.Equal(sweepEpoch) {
		t.Fatalf("clock: got %v, want %v", got, sweepEpoch)
	}
}

func TestExpiredLeaseArgsRefusesAPageOutsideTheCeiling(t *testing.T) {
	for _, limit := range []int{-1, maxExpiredLeasePage + 1} {
		if _, err := expiredLeaseArgs(sweepEpoch, limit); !errors.Is(err, ErrInvalid) {
			t.Fatalf("limit %d: got %v, want ErrInvalid", limit, err)
		}
	}
	if _, err := expiredLeaseArgs(sweepEpoch, maxExpiredLeasePage); err != nil {
		t.Fatalf("limit at the ceiling: %v", err)
	}
}

func expiredRow() expiredLeaseRow {
	return expiredLeaseRow{
		BoardID:   "ek-ra8d2-01",
		LeaseID:   "9a2c0a7e-0f0a-4a1f-9a54-9c0b9f2e1d33",
		Holder:    "operator@lab",
		ExpiresAt: sweepEpoch.Add(-time.Minute),
		Version:   14,
	}
}

func TestExpiredLeaseFromCarriesTheVersionTheTickAppliesUnder(t *testing.T) {
	lease, err := expiredLeaseFrom(expiredRow(), sweepEpoch)
	if err != nil {
		t.Fatalf("row: %v", err)
	}
	if lease.Version != 14 {
		t.Fatalf("version: got %d, want 14", lease.Version)
	}
	if lease.BoardID != "ek-ra8d2-01" || lease.Holder != "operator@lab" {
		t.Fatalf("identity: got %+v", lease)
	}
	if !lease.ExpiresAt.Equal(sweepEpoch.Add(-time.Minute)) {
		t.Fatalf("deadline: got %v", lease.ExpiresAt)
	}
}

// The deadline is judged a second time on the value that came back. A read
// whose whole purpose is expiry must never hand a live lease to the sweep.
func TestExpiredLeaseFromRefusesALeaseThatHasNotExpired(t *testing.T) {
	row := expiredRow()
	row.ExpiresAt = sweepEpoch.Add(time.Second)
	if _, err := expiredLeaseFrom(row, sweepEpoch); !errors.Is(err, ErrConflict) {
		t.Fatalf("live lease: got %v, want ErrConflict", err)
	}
}

// Exactly at the deadline is expired: board.Apply expires on !now.Before(expiry)
// and the two judgements must not disagree about the boundary instant.
func TestExpiredLeaseFromTakesTheDeadlineInstantAsExpired(t *testing.T) {
	row := expiredRow()
	row.ExpiresAt = sweepEpoch
	if _, err := expiredLeaseFrom(row, sweepEpoch); err != nil {
		t.Fatalf("deadline instant: %v", err)
	}
}

func TestExpiredLeaseFromRefusesARowWithNoIdentity(t *testing.T) {
	for name, mutate := range map[string]func(*expiredLeaseRow){
		"board":  func(r *expiredLeaseRow) { r.BoardID = "" },
		"lease":  func(r *expiredLeaseRow) { r.LeaseID = "" },
		"holder": func(r *expiredLeaseRow) { r.Holder = "" },
	} {
		row := expiredRow()
		mutate(&row)
		if _, err := expiredLeaseFrom(row, sweepEpoch); !errors.Is(err, ErrConflict) {
			t.Fatalf("%s: got %v, want ErrConflict", name, err)
		}
	}
}

// Version zero would make the tick apply under an expectation the board
// cannot be at, and the sweep would re-earn that conflict every pass.
func TestExpiredLeaseFromRefusesAHeldBoardAtVersionZero(t *testing.T) {
	row := expiredRow()
	row.Version = 0
	if _, err := expiredLeaseFrom(row, sweepEpoch); !errors.Is(err, ErrConflict) {
		t.Fatalf("version zero: got %v, want ErrConflict", err)
	}
}

func TestExpiredLeaseFromNormalisesTheDeadlineToUTC(t *testing.T) {
	zone := time.FixedZone("lab", 3*3600)
	row := expiredRow()
	row.ExpiresAt = sweepEpoch.Add(-time.Hour).In(zone)
	lease, err := expiredLeaseFrom(row, sweepEpoch)
	if err != nil {
		t.Fatalf("row: %v", err)
	}
	if lease.ExpiresAt.Location() != time.UTC {
		t.Fatalf("zone: got %v, want UTC", lease.ExpiresAt.Location())
	}
}
