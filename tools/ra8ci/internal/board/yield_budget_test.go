package board

import (
	"strings"
	"testing"
	"time"
)

func testCohort() YieldCohort {
	return YieldCohort{
		BoardID:         "ra8p1-bench-1",
		BoardModel:      "ra8p1",
		FixtureRevision: "fixture-c",
		TaskName:        "hil-smoke",
		CatalogDigest:   "2411656a6225954d8f8b6b4a593a79b0b95ddd080c5040515cb0a227f65216e5",
		ImageSHA256:     "0e6f1d5b",
	}
}

func testBounds() DeclaredHandoffBounds {
	return DeclaredHandoffBounds{SafeStepBound: 12 * time.Second, RestoreProbeBound: 8 * time.Second}
}

// completedSamples builds n comparable handoffs whose latencies are
// seconds[i], all recent relative to now.
func completedSamples(cohort YieldCohort, now time.Time, seconds ...int) []YieldSample {
	samples := make([]YieldSample, 0, len(seconds))
	for i, s := range seconds {
		requested := now.Add(-time.Duration(len(seconds)-i) * time.Hour)
		samples = append(samples, YieldSample{
			Cohort:      cohort,
			RequestedAt: requested,
			NeutralAt:   requested.Add(time.Duration(s) * time.Second),
		})
	}
	return samples
}

func TestHandoffFallsBackToDeclaredBoundsBelowTheSampleFloor(t *testing.T) {
	now := time.Now()
	cohort, bounds := testCohort(), testBounds()

	for count := 0; count < MinimumHandoffSamples; count++ {
		seconds := make([]int, count)
		for i := range seconds {
			seconds[i] = 3
		}
		estimate, err := EstimateHandoff(cohort, bounds, completedSamples(cohort, now, seconds...), now)
		if err != nil {
			t.Fatalf("%d samples: %v", count, err)
		}
		if estimate.Source != HandoffFromDeclaredBounds || estimate.Samples != count {
			t.Fatalf("%d samples: source %s, samples %d", count, estimate.Source, estimate.Samples)
		}
		if estimate.Target != bounds.SafetyBound() {
			t.Fatalf("%d samples: target = %s, want the declared %s", count, estimate.Target, bounds.SafetyBound())
		}
		// Thirty seconds is a handoff target for a declared cancel-safe
		// experiment, never a stand-in for a bound this task declared.
		if estimate.Target == DefaultHandoffTarget && bounds.SafetyBound() != DefaultHandoffTarget {
			t.Fatalf("%d samples: fell back to the default grace", count)
		}
		if estimate.Margin != 0 {
			t.Fatalf("%d samples: declared estimate carries a margin %s", count, estimate.Margin)
		}
	}
}

func TestHandoffUsesAConservativeQuantilePlusMargin(t *testing.T) {
	now := time.Now()
	cohort := testCohort()
	// Twenty samples, so the nearest rank at p95 is index ceil(0.95*20)-1 = 18,
	// the second largest: 40s. A mean or median would answer far lower, and an
	// interpolated quantile would invent a latency between 40s and 90s.
	seconds := []int{5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 12, 13, 14, 16, 20, 25, 40, 90}
	estimate, err := EstimateHandoff(cohort, testBounds(), completedSamples(cohort, now, seconds...), now)
	if err != nil {
		t.Fatal(err)
	}
	want := 40*time.Second + HandoffMargin
	if estimate.Source != HandoffFromHistory || estimate.Target != want {
		t.Fatalf("target = %s from %s, want %s from history", estimate.Target, estimate.Source, want)
	}
	if estimate.Samples != len(seconds) || estimate.Margin != HandoffMargin || estimate.Quantile != HandoffQuantile {
		t.Fatalf("provenance not reported: %#v", estimate)
	}
	if estimate.NewestSample.Before(estimate.OldestSample) || estimate.NewestSample.After(now) {
		t.Fatalf("sample ages wrong: oldest %s newest %s", estimate.OldestSample, estimate.NewestSample)
	}
}

func TestHandoffNeverShortensADeclaredSafetyBound(t *testing.T) {
	now := time.Now()
	cohort := testCohort()
	// A task that declares a 4 minute indivisible flash plus a 1 minute
	// restore keeps that bound even though every observed handoff was quick:
	// the history describes handoffs that happened to land between flashes.
	bounds := DeclaredHandoffBounds{SafeStepBound: 4 * time.Minute, RestoreProbeBound: time.Minute}
	estimate, err := EstimateHandoff(cohort, bounds, completedSamples(cohort, now, 2, 2, 3, 3, 4, 4, 5, 5), now)
	if err != nil {
		t.Fatal(err)
	}
	if estimate.Target != bounds.SafetyBound() {
		t.Fatalf("target = %s, estimator shortened the declared %s", estimate.Target, bounds.SafetyBound())
	}
	if estimate.Source != HandoffFromHistory || estimate.Samples != 8 || estimate.SafetyBound != bounds.SafetyBound() {
		t.Fatalf("history was discarded rather than floored: %#v", estimate)
	}

	// A clamp at the estimator's maximum may not shorten a declared bound
	// either: a cohort at the ceiling stays at the ceiling.
	ceiling := DeclaredHandoffBounds{SafeStepBound: MaxHandoffBound - time.Minute, RestoreProbeBound: time.Minute}
	estimate, err = EstimateHandoff(cohort, ceiling, completedSamples(cohort, now, 1, 1, 1, 1, 1, 1), now)
	if err != nil {
		t.Fatal(err)
	}
	if estimate.Target != MaxHandoffBound {
		t.Fatalf("target = %s, want the declared ceiling %s", estimate.Target, MaxHandoffBound)
	}
}

func TestHandoffKeepsCensoredAndStaleRowsOutOfTheLatency(t *testing.T) {
	now := time.Now()
	cohort, bounds := testCohort(), testBounds()

	samples := completedSamples(cohort, now, 6, 6, 7)
	// Retained, never measured: a request that never reached a neutral board
	// has no neutral time at all.
	for i := 0; i < 4; i++ {
		samples = append(samples, YieldSample{
			Cohort:          cohort,
			RequestedAt:     now.Add(-2 * time.Hour),
			ExclusionReason: "holder died before neutral",
		})
	}
	stale := now.Add(-MaxHandoffSampleAge - time.Hour)
	for i := 0; i < 4; i++ {
		samples = append(samples, YieldSample{Cohort: cohort, RequestedAt: stale, NeutralAt: stale.Add(3 * time.Second)})
	}
	// Another cohort's fast handoffs must not lend this one a history.
	foreign := cohort
	foreign.FixtureRevision = "fixture-d"
	samples = append(samples, completedSamples(foreign, now, 1, 1, 1, 1, 1, 1, 1, 1)...)

	estimate, err := EstimateHandoff(cohort, bounds, samples, now)
	if err != nil {
		t.Fatal(err)
	}
	if estimate.Samples != 3 || estimate.Censored != 4 || estimate.Stale != 4 {
		t.Fatalf("counted wrong: %d samples, %d censored, %d stale", estimate.Samples, estimate.Censored, estimate.Stale)
	}
	if estimate.Source != HandoffFromDeclaredBounds || estimate.Target != bounds.SafetyBound() {
		t.Fatalf("censored, stale or foreign rows reached the floor: %#v", estimate)
	}
}

func TestHandoffKeepsSafetyOverrunsInTheEstimate(t *testing.T) {
	now := time.Now()
	cohort := testCohort()
	samples := completedSamples(cohort, now, 5, 5, 6, 6, 7, 7, 8, 120)
	samples[len(samples)-1].SafetyOverrun = true

	estimate, err := EstimateHandoff(cohort, testBounds(), samples, now)
	if err != nil {
		t.Fatal(err)
	}
	if estimate.Overruns != 1 || estimate.Samples != len(samples) {
		t.Fatalf("overrun dropped: %d overruns over %d samples", estimate.Overruns, estimate.Samples)
	}
	if estimate.Target != 120*time.Second+HandoffMargin {
		t.Fatalf("target = %s, overrun did not raise the estimate", estimate.Target)
	}
}

func TestHandoffRefusesContradictoryRowsAndUndeclaredTasks(t *testing.T) {
	now := time.Now()
	cohort, bounds := testCohort(), testBounds()

	cases := map[string]YieldSample{
		"no times":         {Cohort: cohort},
		"no neutral time":  {Cohort: cohort, RequestedAt: now.Add(-time.Minute)},
		"neutral first":    {Cohort: cohort, RequestedAt: now.Add(-time.Minute), NeutralAt: now.Add(-2 * time.Minute)},
		"neutral in front": {Cohort: cohort, RequestedAt: now, NeutralAt: now.Add(time.Hour)},
		"long reason":      {Cohort: cohort, ExclusionReason: strings.Repeat("x", maxExclusionBytes+1)},
	}
	for name, sample := range cases {
		if _, err := EstimateHandoff(cohort, bounds, []YieldSample{sample}, now); !IsCode(err, InvalidArgument) {
			t.Fatalf("%s accepted: %v", name, err)
		}
	}

	// A task with no declared bounds has an unknown ETA; automatic dispatch is
	// rejected rather than answered with the default grace.
	for _, undeclared := range []DeclaredHandoffBounds{
		{},
		{SafeStepBound: 5 * time.Second},
		{RestoreProbeBound: 5 * time.Second},
		{SafeStepBound: MaxHandoffBound, RestoreProbeBound: time.Second},
	} {
		if _, err := EstimateHandoff(cohort, undeclared, completedSamples(cohort, now, 3, 3, 3, 3, 3, 3), now); !IsCode(err, InvalidArgument) {
			t.Fatalf("undeclared bounds %#v accepted: %v", undeclared, err)
		}
	}

	if _, err := EstimateHandoff(YieldCohort{BoardID: "b"}, bounds, nil, now); !IsCode(err, InvalidArgument) {
		t.Fatalf("incomplete cohort accepted: %v", err)
	}
	if _, err := EstimateHandoff(cohort, bounds, nil, time.Time{}); !IsCode(err, InvalidArgument) {
		t.Fatalf("zero clock accepted: %v", err)
	}
	if _, err := EstimateHandoff(cohort, bounds, make([]YieldSample, MaxHandoffSamples+1), now); !IsCode(err, InvalidArgument) {
		t.Fatalf("unbounded history accepted: %v", err)
	}
}

func TestHandoffProvenanceNamesCohortSamplesAndAge(t *testing.T) {
	now := time.Now()
	cohort := testCohort()
	line := mustEstimate(t, cohort, testBounds(), completedSamples(cohort, now, 4, 4, 5, 5, 6, 6), now).Provenance(now)
	for _, want := range []string{"p95", "6 samples", "old", cohort.BoardID, cohort.FixtureRevision, cohort.TaskName} {
		if !strings.Contains(line, want) {
			t.Fatalf("provenance %q omits %q", line, want)
		}
	}
	line = mustEstimate(t, cohort, testBounds(), nil, now).Provenance(now)
	if !strings.Contains(line, "declared bounds") || strings.Contains(line, "p95") {
		t.Fatalf("declared provenance claims history: %q", line)
	}
}

func mustEstimate(t *testing.T, cohort YieldCohort, bounds DeclaredHandoffBounds, samples []YieldSample, now time.Time) HandoffEstimate {
	t.Helper()
	estimate, err := EstimateHandoff(cohort, bounds, samples, now)
	if err != nil {
		t.Fatal(err)
	}
	return estimate
}
