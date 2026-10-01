package store

import (
	"errors"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
)

func historyCohort() board.YieldCohort {
	return board.YieldCohort{
		BoardID:         "ra8p1-03",
		BoardModel:      "ra8p1-evk",
		FixtureRevision: "fixture-7",
		TaskName:        "hil-smoke",
		CatalogDigest:   "sha256:catalog",
		ImageSHA256:     "sha256:image",
	}
}

func measuredRow(now time.Time) yieldSampleRow {
	return yieldSampleRow{
		LeaseID:     "11111111-1111-1111-1111-111111111111",
		BoardID:     "ra8p1-03",
		WaiterID:    "22222222-2222-2222-2222-222222222222",
		Cohort:      historyCohort(),
		RequestedAt: now.Add(-2 * time.Minute),
		NeutralAt:   now.Add(-90 * time.Second),
		ShownTarget: 45 * time.Second,
	}
}

// The read's age filter and the estimator's staleness judgement measure from
// different columns, so the read has to be the wider of the two or it drops
// history the estimator would have counted.
func TestYieldHistoryReadsBackEverySampleTheEstimatorWouldCount(t *testing.T) {
	now := time.Date(2026, 9, 24, 18, 0, 0, 0, time.UTC)
	cutoff := yieldHistoryCutoff(now)

	// The slowest handoff the estimator still counts, requested as long ago
	// as it can be while reaching neutral inside the sample age.
	requested := now.Add(-board.MaxHandoffSampleAge).Add(-board.MaxHandoffBound).Add(time.Second)
	if requested.Before(cutoff) {
		t.Fatalf("read excludes a handoff the estimator counts: requested %s, cutoff %s", requested, cutoff)
	}
	sample := board.YieldSample{
		Cohort:      historyCohort(),
		LeaseID:     "lease",
		RequestedAt: requested,
		NeutralAt:   requested.Add(board.MaxHandoffBound),
	}
	estimate, err := board.EstimateHandoff(historyCohort(),
		board.DeclaredHandoffBounds{SafeStepBound: 10 * time.Second, RestoreProbeBound: 5 * time.Second},
		[]board.YieldSample{sample}, now)
	if err != nil {
		t.Fatalf("estimate: %v", err)
	}
	if estimate.Samples != 1 || estimate.Stale != 0 {
		t.Fatalf("estimator did not count the boundary sample: samples %d stale %d", estimate.Samples, estimate.Stale)
	}
}

func TestYieldHistoryArgsCarryTheWholeCohortAndTheBounds(t *testing.T) {
	now := time.Date(2026, 9, 24, 18, 0, 0, 0, time.UTC)
	args, err := yieldHistoryArgs(historyCohort(), now)
	if err != nil {
		t.Fatalf("args: %v", err)
	}
	want := []any{"ra8p1-03", "ra8p1-evk", "fixture-7", "hil-smoke", "sha256:catalog", "sha256:image",
		yieldHistoryCutoff(now), yieldHistoryPageSize}
	if len(args) != len(want) {
		t.Fatalf("argument count %d, want %d", len(args), len(want))
	}
	for i := range want {
		if args[i] != want[i] {
			t.Fatalf("argument %d is %v, want %v", i, args[i], want[i])
		}
	}
	if yieldHistoryPageSize != board.MaxHandoffSamples {
		t.Fatalf("page size %d must match the estimator's bound %d", yieldHistoryPageSize, board.MaxHandoffSamples)
	}
}

// An imageless task is its own cohort, so the empty digest has to reach the
// query as a value rather than turning the read into a wildcard.
func TestYieldHistoryArgsKeepAnEmptyImageDigestAsIdentity(t *testing.T) {
	cohort := historyCohort()
	cohort.ImageSHA256 = ""
	args, err := yieldHistoryArgs(cohort, time.Date(2026, 9, 24, 18, 0, 0, 0, time.UTC))
	if err != nil {
		t.Fatalf("args: %v", err)
	}
	if args[5] != "" {
		t.Fatalf("image digest argument is %v, want the empty string", args[5])
	}
}

func TestYieldHistoryArgsRefuseAnIncompleteRead(t *testing.T) {
	now := time.Date(2026, 9, 24, 18, 0, 0, 0, time.UTC)
	partial := historyCohort()
	partial.TaskName = ""
	if _, err := yieldHistoryArgs(partial, now); err == nil {
		t.Fatal("a cohort missing its task name was read anyway")
	}
	if _, err := yieldHistoryArgs(historyCohort(), time.Time{}); !errors.Is(err, ErrInvalid) {
		t.Fatalf("zero clock: %v, want %v", err, ErrInvalid)
	}
}

func TestYieldSampleFromCarriesTheStoredMeasurement(t *testing.T) {
	now := time.Date(2026, 9, 24, 18, 0, 0, 0, time.UTC)
	row := measuredRow(now)
	row.SafetyOverrun = true
	sample, err := yieldSampleFrom(row, historyCohort())
	if err != nil {
		t.Fatalf("decode: %v", err)
	}
	if sample.Cohort != historyCohort() || sample.LeaseID != row.LeaseID || sample.WaiterID != row.WaiterID {
		t.Fatalf("identity not carried: %+v", sample)
	}
	if !sample.RequestedAt.Equal(row.RequestedAt) || !sample.NeutralAt.Equal(row.NeutralAt) {
		t.Fatalf("times not carried: %+v", sample)
	}
	if !sample.SafetyOverrun || sample.ExclusionReason != "" || !sample.Completed() {
		t.Fatalf("outcome not carried: %+v", sample)
	}
	if sample.Latency() != 30*time.Second {
		t.Fatalf("latency %s, want 30s", sample.Latency())
	}
}

func TestYieldSampleFromKeepsACensoredRowInTheHistory(t *testing.T) {
	now := time.Date(2026, 9, 24, 18, 0, 0, 0, time.UTC)
	row := measuredRow(now)
	row.NeutralAt = time.Time{}
	row.ExclusionReason = board.YieldExcludedNoReceipt
	sample, err := yieldSampleFrom(row, historyCohort())
	if err != nil {
		t.Fatalf("decode: %v", err)
	}
	if sample.Completed() || sample.ExclusionReason != board.YieldExcludedNoReceipt {
		t.Fatalf("censored row decoded as a measurement: %+v", sample)
	}
}

// The query selects one cohort by equality on all six columns, so a row under
// a different one means the read and the row disagree. The estimator would
// skip it silently; the read says so.
func TestYieldSampleFromRefusesARowOutsideTheCohortRead(t *testing.T) {
	now := time.Date(2026, 9, 24, 18, 0, 0, 0, time.UTC)
	for _, drift := range []func(*board.YieldCohort){
		func(c *board.YieldCohort) { c.BoardID = "ra8p1-04" },
		func(c *board.YieldCohort) { c.BoardModel = "ra8p1-rev-b" },
		func(c *board.YieldCohort) { c.FixtureRevision = "fixture-8" },
		func(c *board.YieldCohort) { c.TaskName = "hil-soak" },
		func(c *board.YieldCohort) { c.CatalogDigest = "sha256:other" },
		func(c *board.YieldCohort) { c.ImageSHA256 = "" },
	} {
		row := measuredRow(now)
		drift(&row.Cohort)
		if _, err := yieldSampleFrom(row, historyCohort()); !errors.Is(err, ErrConflict) {
			t.Fatalf("cohort %+v was read into the wrong history: %v", row.Cohort, err)
		}
	}
}

func TestYieldSampleFromRefusesARowThatIsNeitherMeasuredNorCensored(t *testing.T) {
	now := time.Date(2026, 9, 24, 18, 0, 0, 0, time.UTC)

	both := measuredRow(now)
	both.ExclusionReason = board.YieldExcludedExpired
	if _, err := yieldSampleFrom(both, historyCohort()); !errors.Is(err, ErrConflict) {
		t.Fatalf("a row that is both was accepted: %v", err)
	}

	neither := measuredRow(now)
	neither.NeutralAt = time.Time{}
	if _, err := yieldSampleFrom(neither, historyCohort()); !errors.Is(err, ErrConflict) {
		t.Fatalf("a row that is neither was accepted: %v", err)
	}
}

func TestYieldSampleFromRefusesAnImpossibleStoredOutcome(t *testing.T) {
	now := time.Date(2026, 9, 24, 18, 0, 0, 0, time.UTC)

	overrun := measuredRow(now)
	overrun.NeutralAt = time.Time{}
	overrun.ExclusionReason = board.YieldExcludedWithdrawn
	overrun.SafetyOverrun = true
	if _, err := yieldSampleFrom(overrun, historyCohort()); !errors.Is(err, ErrConflict) {
		t.Fatalf("a censored row claiming an overrun was accepted: %v", err)
	}

	backwards := measuredRow(now)
	backwards.NeutralAt = backwards.RequestedAt.Add(-time.Second)
	if _, err := yieldSampleFrom(backwards, historyCohort()); !errors.Is(err, ErrConflict) {
		t.Fatalf("a row reaching neutral before its request was accepted: %v", err)
	}

	anonymous := measuredRow(now)
	anonymous.LeaseID = ""
	if _, err := yieldSampleFrom(anonymous, historyCohort()); !errors.Is(err, ErrConflict) {
		t.Fatalf("a row with no lease was accepted: %v", err)
	}
}

// A refusal names the lease so a bad row can be found, and carries nothing
// else about the work.
func TestYieldSampleFromNamesTheOffendingLease(t *testing.T) {
	now := time.Date(2026, 9, 24, 18, 0, 0, 0, time.UTC)
	row := measuredRow(now)
	row.ExclusionReason = board.YieldExcludedRecovery
	_, err := yieldSampleFrom(row, historyCohort())
	if err == nil || !strings.Contains(err.Error(), row.LeaseID) {
		t.Fatalf("refusal %v does not name lease %s", err, row.LeaseID)
	}
}

// What the recorder writes is what the read hands back, over the whole loop:
// a sample derived from a committed transition, shaped into its row, read
// back, and estimated over.
func TestRecordedSamplesEstimateAfterAReadBack(t *testing.T) {
	now := time.Date(2026, 9, 24, 18, 0, 0, 0, time.UTC)
	samples := make([]board.YieldSample, 0, board.MinimumHandoffSamples)
	for i := 0; i < board.MinimumHandoffSamples; i++ {
		row := measuredRow(now)
		row.LeaseID = string(rune('a'+i)) + "-lease"
		row.RequestedAt = now.Add(-time.Duration(i+1) * time.Hour)
		row.NeutralAt = row.RequestedAt.Add(time.Duration(20+i) * time.Second)
		sample, err := yieldSampleFrom(row, historyCohort())
		if err != nil {
			t.Fatalf("decode %d: %v", i, err)
		}
		samples = append(samples, sample)
	}
	estimate, err := board.EstimateHandoff(historyCohort(),
		board.DeclaredHandoffBounds{SafeStepBound: 5 * time.Second, RestoreProbeBound: 5 * time.Second},
		samples, now)
	if err != nil {
		t.Fatalf("estimate: %v", err)
	}
	if estimate.Source != board.HandoffFromHistory {
		t.Fatalf("source %q, want history once the floor is met", estimate.Source)
	}
	if estimate.Samples != board.MinimumHandoffSamples {
		t.Fatalf("samples %d, want %d", estimate.Samples, board.MinimumHandoffSamples)
	}
	if estimate.Target != 24*time.Second+board.HandoffMargin {
		t.Fatalf("target %s, want p95 of the read-back latencies plus the margin", estimate.Target)
	}
}
