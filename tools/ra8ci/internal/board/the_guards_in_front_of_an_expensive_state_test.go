package board

import (
	"strings"
	"testing"
	"time"
)

// Two guards that sit in front of state a board cannot take back: the
// identity an agent's generation report must carry before it can quarantine
// the board, and the field lengths a cohort must fit inside before its
// history is filed under it. Both are cheap checks in front of expensive
// consequences, which is exactly the shape that goes unnoticed when it drifts.

func observedBoard(t *testing.T) Snapshot {
	t.Helper()
	s, err := New("board-observed")
	if err != nil {
		t.Fatalf("new board: %v", err)
	}
	return s
}

func TestAGenerationReportWithNoAgentIdentityCannotQuarantineTheBoard(t *testing.T) {
	// The missing-identity guard runs AHEAD of the high-water comparison,
	// and that order is the whole point. A high-water the database never
	// issued is the restored-from-backup signal and it quarantines the
	// board, which is a phase only a reviewed recovery sequence leaves.
	// The quarantine event records who reported it; a nameless report
	// would put the board out of service with nothing in the audit trail
	// naming what observed the contradiction.
	s := observedBoard(t)
	before := s

	after, events, err := Apply(s, ObserveAgentGeneration{HighWater: s.Generation + 5}, testEpoch)
	if !IsCode(err, InvalidArgument) {
		t.Fatalf("nameless observation not refused: %v", err)
	}
	if after.Phase != before.Phase {
		t.Fatalf("refused observation moved the phase: %v", after.Phase)
	}
	if after.AgentHighWater != before.AgentHighWater {
		t.Fatalf("refused observation moved the high-water: %d", after.AgentHighWater)
	}
	for _, e := range events {
		if e.Kind == BoardQuarantined {
			t.Fatal("a nameless report quarantined the board")
		}
	}

	// The same contradiction from a named agent does quarantine it, so the
	// refusal above is about the missing name and nothing else.
	named, _, err := Apply(s, ObserveAgentGeneration{Actor: "board-agent", HighWater: s.Generation + 5}, testEpoch)
	if err != nil {
		t.Fatalf("named observation refused: %v", err)
	}
	if named.Phase != Quarantined {
		t.Fatalf("named contradiction did not quarantine: %v", named.Phase)
	}
}

func TestAnAgreeingReportWithNoIdentityIsRefusedJustTheSame(t *testing.T) {
	// A report that agrees with the board changes nothing, so refusing it
	// costs nothing either. The guard is unconditional rather than a
	// special case around the damaging branch, which is what keeps it
	// correct when a new branch is added below it.
	s := observedBoard(t)
	if _, _, err := Apply(s, ObserveAgentGeneration{HighWater: s.AgentHighWater}, testEpoch); !IsCode(err, InvalidArgument) {
		t.Fatalf("nameless agreeing observation not refused: %v", err)
	}
}

func TestEveryCohortFieldIsCappedAtTheSameLength(t *testing.T) {
	// A cohort is the bucket a handoff measurement is filed under and is
	// carried on the lease itself, so an unbounded field is an unbounded
	// row in every store that persists one. The cap is the same for all
	// six, and the refusal names the field so the caller knows which one.
	fits := strings.Repeat("x", maxCohortFieldBytes)
	tooLong := strings.Repeat("x", maxCohortFieldBytes+1)
	cases := []struct {
		field string
		set   func(*YieldCohort, string)
	}{
		{"board ID", func(c *YieldCohort, v string) { c.BoardID = v }},
		{"board model", func(c *YieldCohort, v string) { c.BoardModel = v }},
		{"fixture revision", func(c *YieldCohort, v string) { c.FixtureRevision = v }},
		{"task name", func(c *YieldCohort, v string) { c.TaskName = v }},
		{"catalog digest", func(c *YieldCohort, v string) { c.CatalogDigest = v }},
		{"image digest", func(c *YieldCohort, v string) { c.ImageSHA256 = v }},
	}
	for _, tc := range cases {
		atCap := testCohort()
		tc.set(&atCap, fits)
		if err := ValidateYieldCohort(atCap); err != nil {
			t.Fatalf("%s at the cap refused: %v", tc.field, err)
		}

		over := testCohort()
		tc.set(&over, tooLong)
		err := ValidateYieldCohort(over)
		if !IsCode(err, InvalidArgument) {
			t.Fatalf("%s one byte over the cap admitted: %v", tc.field, err)
		}
		want := "yield cohort " + tc.field + " is too long"
		if !strings.Contains(err.Error(), want) {
			t.Fatalf("%s: wrong refusal %q", tc.field, err)
		}
	}
}

func TestTheImageDigestIsLengthCheckedThoughItMayBeEmpty(t *testing.T) {
	// The image digest is the one optional field: a task that flashes no
	// image has none. Optional is not unchecked, and empty is still part
	// of the identity rather than a wildcard.
	imageless := testCohort()
	imageless.ImageSHA256 = ""
	if err := ValidateYieldCohort(imageless); err != nil {
		t.Fatalf("imageless cohort refused: %v", err)
	}
	over := testCohort()
	over.ImageSHA256 = strings.Repeat("d", maxCohortFieldBytes+1)
	if err := ValidateYieldCohort(over); !IsCode(err, InvalidArgument) {
		t.Fatalf("overlong image digest admitted: %v", err)
	}
}

func TestAMissingCohortFieldIsReportedAheadOfAnOverlongOne(t *testing.T) {
	// Both loops run over the same list, the missing check first. A cohort
	// wrong in both ways reads as the missing field, which is the one the
	// caller has to supply before the length even matters.
	cohort := testCohort()
	cohort.BoardModel = ""
	cohort.TaskName = strings.Repeat("t", maxCohortFieldBytes+1)
	err := ValidateYieldCohort(cohort)
	if !IsCode(err, InvalidArgument) {
		t.Fatalf("not refused: %v", err)
	}
	if !strings.Contains(err.Error(), "missing its board model") {
		t.Fatalf("wrong refusal: %v", err)
	}
}

func TestNoHistoryPushesTheETAPastTheEstimatorsMaximum(t *testing.T) {
	// MaxHandoffBound is the ceiling on what anyone may be promised, and
	// two separate clamps hold it: one on each measured latency as it is
	// read, one on the quantile plus margin at the end. An hour-long
	// handoff is already a board nobody should be waiting on, and a
	// promise past the ceiling would be a number the requester is shown
	// and the scheduler then plans around.
	now := testEpoch
	cohort, bounds := testCohort(), testBounds()

	overBound := func(latency time.Duration, count int) []YieldSample {
		samples := make([]YieldSample, 0, count)
		for i := 0; i < count; i++ {
			requested := now.Add(-time.Duration(count-i) * 3 * time.Hour)
			samples = append(samples, YieldSample{
				Cohort:      cohort,
				RequestedAt: requested,
				NeutralAt:   requested.Add(latency),
			})
		}
		return samples
	}

	for _, latency := range []time.Duration{MaxHandoffBound, MaxHandoffBound + time.Second, 3 * MaxHandoffBound} {
		estimate := mustEstimate(t, cohort, bounds, overBound(latency, 8), now)
		if estimate.Source != HandoffFromHistory {
			t.Fatalf("%v: estimate did not come from history: %v", latency, estimate.Source)
		}
		if estimate.Target != MaxHandoffBound {
			t.Fatalf("%v: target is not the ceiling: %v", latency, estimate.Target)
		}
	}

	// The clamp is a ceiling, not a floor: a history that sits under it is
	// reported as measured, margin included, rather than rounded up to it.
	ordinary := mustEstimate(t, cohort, bounds, completedSamples(cohort, now, 30, 30, 30, 30, 30, 30), now)
	if ordinary.Target != 30*time.Second+HandoffMargin {
		t.Fatalf("an ordinary history was clamped: %v", ordinary.Target)
	}
}

func TestOneRunawayHandoffCannotDragThePromiseToTheCeiling(t *testing.T) {
	// The other side of the same rule. The quantile is what keeps a single
	// runaway out of the number, so a history of quick handoffs with one
	// hour-long outlier is still reported as quick: the clamp bounds the
	// worst case, it does not become the answer.
	now := testEpoch
	cohort, bounds := testCohort(), testBounds()

	latencies := make([]time.Duration, 0, 20)
	for i := 0; i < 19; i++ {
		latencies = append(latencies, 25*time.Second)
	}
	latencies = append(latencies, 3*MaxHandoffBound)

	samples := make([]YieldSample, 0, len(latencies))
	for i, latency := range latencies {
		// Spaced a day apart so even the runaway reaches neutral well
		// before now, and all of it inside MaxHandoffSampleAge.
		requested := now.Add(-time.Duration(len(latencies)-i) * 24 * time.Hour)
		samples = append(samples, YieldSample{
			Cohort:      cohort,
			RequestedAt: requested,
			NeutralAt:   requested.Add(latency),
		})
	}

	estimate := mustEstimate(t, cohort, bounds, samples, now)
	if estimate.Source != HandoffFromHistory {
		t.Fatalf("estimate did not come from history: %v", estimate.Source)
	}
	if estimate.Samples != len(latencies) {
		t.Fatalf("history was not all measured: %d", estimate.Samples)
	}
	if estimate.Target != 25*time.Second+HandoffMargin {
		t.Fatalf("one outlier moved the promise: %v", estimate.Target)
	}
}
