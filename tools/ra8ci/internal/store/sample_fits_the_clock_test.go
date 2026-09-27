package store

import (
	"errors"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
)

func clockNow() time.Time {
	return time.Date(2026, 9, 26, 18, 0, 0, 0, time.UTC)
}

// censoredRow is a retained row for a request that never reached neutral: no
// neutral stamp, a reason instead. It is the shape the estimator exempts.
func censoredRow(requested time.Time) yieldSampleRow {
	return yieldSampleRow{
		LeaseID:         "33333333-3333-3333-3333-333333333333",
		BoardID:         "ra8p1-03",
		Cohort:          historyCohort(),
		RequestedAt:     requested,
		ExclusionReason: "holder never reached neutral",
	}
}

func TestClockAcceptsAnOrdinaryPastSample(t *testing.T) {
	now := clockNow()
	if err := yieldSampleFitsTheClock(measuredRow(now), now); err != nil {
		t.Fatalf("ordinary sample refused: %v", err)
	}
}

func TestClockAcceptsStampsAtTheReadingMoment(t *testing.T) {
	now := clockNow()
	row := measuredRow(now)
	row.RequestedAt = now
	row.NeutralAt = now
	if err := yieldSampleFitsTheClock(row, now); err != nil {
		t.Fatalf("sample stamped at now refused: %v", err)
	}
}

// The tolerance is the plane's own, not a number invented here.
func TestClockToleranceIsThePlanesClockOffset(t *testing.T) {
	now := clockNow()
	row := measuredRow(now)
	row.RequestedAt = now.Add(board.MaxClockOffset)
	row.NeutralAt = now.Add(board.MaxClockOffset)
	if err := yieldSampleFitsTheClock(row, now); err != nil {
		t.Fatalf("sample at the offset bound refused: %v", err)
	}
	row.NeutralAt = now.Add(board.MaxClockOffset + time.Nanosecond)
	if err := yieldSampleFitsTheClock(row, now); err == nil {
		t.Fatal("sample one nanosecond past the offset bound accepted")
	}
}

func TestClockRefusesARequestStampAheadOfTheReader(t *testing.T) {
	now := clockNow()
	row := measuredRow(now)
	row.RequestedAt = now.Add(time.Hour)
	row.NeutralAt = now.Add(2 * time.Hour)
	err := yieldSampleFitsTheClock(row, now)
	if !errors.Is(err, ErrConflict) {
		t.Fatalf("want ErrConflict, got %v", err)
	}
	if !strings.Contains(err.Error(), row.LeaseID) {
		t.Fatalf("refusal does not name the row: %v", err)
	}
	if !strings.Contains(err.Error(), "requested in the future") {
		t.Fatalf("refusal does not say which stamp: %v", err)
	}
}

func TestClockRefusesANeutralStampAheadOfTheReader(t *testing.T) {
	now := clockNow()
	row := measuredRow(now)
	row.NeutralAt = now.Add(time.Minute)
	err := yieldSampleFitsTheClock(row, now)
	if !errors.Is(err, ErrConflict) {
		t.Fatalf("want ErrConflict, got %v", err)
	}
	if !strings.Contains(err.Error(), "reaches neutral in the future") {
		t.Fatalf("refusal does not say which stamp: %v", err)
	}
}

// The door this rule exists for: a censored row is exempt from every stamp
// check the estimator makes, so nothing else in the seam looks at it.
func TestClockRefusesACensoredRowStampedAhead(t *testing.T) {
	now := clockNow()
	err := yieldSampleFitsTheClock(censoredRow(now.Add(90*24*time.Hour)), now)
	if !errors.Is(err, ErrConflict) {
		t.Fatalf("want ErrConflict, got %v", err)
	}
}

func TestClockAcceptsAnOrdinaryCensoredRow(t *testing.T) {
	now := clockNow()
	if err := yieldSampleFitsTheClock(censoredRow(now.Add(-time.Hour)), now); err != nil {
		t.Fatalf("ordinary censored row refused: %v", err)
	}
}

// Proof the gap is real rather than assumed: the estimator counts a censored
// row stamped three months ahead without complaint, and prints the count.
func TestEstimatorItselfCountsACensoredRowFromTheFuture(t *testing.T) {
	now := clockNow()
	bounds := board.DeclaredHandoffBounds{SafeStepBound: 10 * time.Second, RestoreProbeBound: 5 * time.Second}
	sample := board.YieldSample{
		Cohort:          historyCohort(),
		LeaseID:         "33333333-3333-3333-3333-333333333333",
		RequestedAt:     now.Add(90 * 24 * time.Hour),
		ExclusionReason: "holder never reached neutral",
	}
	estimate, err := board.EstimateHandoff(historyCohort(), bounds, []board.YieldSample{sample}, now)
	if err != nil {
		t.Fatalf("estimate: %v", err)
	}
	if estimate.Censored != 1 {
		t.Fatalf("want the future censored row counted, got %d", estimate.Censored)
	}
}

// The age filter bounds the old side only, which is why the future side has
// to be judged per row rather than left to the query.
func TestReadWindowDoesNotBoundTheFutureSide(t *testing.T) {
	now := clockNow()
	ahead := now.Add(365 * 24 * time.Hour)
	if ahead.Before(yieldHistoryCutoff(now)) {
		t.Fatal("cutoff already excludes a row stamped a year ahead")
	}
}

func TestClockNeedsTheCurrentTime(t *testing.T) {
	if err := yieldSampleFitsTheClock(measuredRow(clockNow()), time.Time{}); !errors.Is(err, ErrInvalid) {
		t.Fatalf("want ErrInvalid, got %v", err)
	}
}

// A zero neutral stamp is the absence of a measurement, never a 1970 stamp to
// judge, so it must not be compared against the horizon at all.
func TestClockIgnoresAnAbsentNeutralStamp(t *testing.T) {
	now := clockNow()
	row := censoredRow(now.Add(-time.Minute))
	row.NeutralAt = time.Time{}
	if err := yieldSampleFitsTheClock(row, now); err != nil {
		t.Fatalf("absent neutral stamp refused: %v", err)
	}
}
