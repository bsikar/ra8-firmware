package board

// The rank a yield ETA is taken at, and the age line it is shown beside.
//
// EstimateHandoff answers one question a person is actually waiting on: how
// long until this board is neutral. The number it gives is a NEAREST-RANK
// quantile over comparable completed handoffs, and the file that produces it
// says why in a comment: "never interpolated: an interpolated quantile invents
// a latency nobody measured". That promise had nothing holding it, and neither
// did the two clamps in nearestRank or either silent reading of sampleAge.
//
// What is pinned here is the arithmetic between the history and the sentence:
// which measured handoff the ETA is, what it takes to move it, and that an age
// is never shown as a negative or as a number taken from a sample that is not
// there.

import (
	"strings"
	"testing"
	"time"
)

// etaBounds are deliberately tiny, so the declared floor never masks what the
// history chose. The floor itself is pinned in yield_budget_test.go.
func etaBounds() DeclaredHandoffBounds {
	return DeclaredHandoffBounds{SafeStepBound: time.Second, RestoreProbeBound: time.Second}
}

func etaSorted(seconds ...int) []time.Duration {
	out := make([]time.Duration, 0, len(seconds))
	for _, s := range seconds {
		out = append(out, time.Duration(s)*time.Second)
	}
	return out
}

// etaRun is n distinct rising latencies, so the rank the estimate lands on is
// readable straight off the index.
func etaRun(n int) []int {
	seconds := make([]int, n)
	for i := range seconds {
		seconds[i] = 10 + 3*i
	}
	return seconds
}

// TestTheQuantileNeverInventsALatencyNobodyMeasured holds the promise the
// estimator's own comment makes. Interpolation is the ordinary way to compute
// a quantile and it is wrong here: the ETA is shown to a person as what this
// fixture does, so it has to be a handoff that happened.
func TestTheQuantileNeverInventsALatencyNobodyMeasured(t *testing.T) {
	now := time.Now()
	cohort := testCohort()
	for n := MinimumHandoffSamples; n <= 40; n++ {
		seconds := etaRun(n)
		measured := make(map[time.Duration]bool, n)
		for _, s := range seconds {
			measured[time.Duration(s)*time.Second] = true
		}
		estimate := mustEstimate(t, cohort, etaBounds(), completedSamples(cohort, now, seconds...), now)
		if estimate.Source != HandoffFromHistory {
			t.Fatalf("%d samples: source %s, want history", n, estimate.Source)
		}
		if !measured[estimate.Target-estimate.Margin] {
			t.Fatalf("%d samples: ETA %s less its %s margin is %s, which nobody measured",
				n, estimate.Target, estimate.Margin, estimate.Target-estimate.Margin)
		}
	}
}

// TestTheRankIsTheCeilingOfTheQuantile fixes which measurement that is. The
// ceiling is the conservative direction: at twenty samples it is the
// second-slowest handoff, not the nineteenth of twenty by interpolation and
// not the median dressed up.
func TestTheRankIsTheCeilingOfTheQuantile(t *testing.T) {
	for _, tc := range []struct{ samples, rank int }{
		{1, 1}, {5, 5}, {10, 10}, {19, 19}, {20, 19}, {21, 20}, {40, 38}, {100, 95},
	} {
		sorted := etaSorted(etaRun(tc.samples)...)
		got := nearestRank(sorted, HandoffQuantile)
		if want := sorted[tc.rank-1]; got != want {
			t.Fatalf("%d samples: p95 = %s, want rank %d (%s)", tc.samples, got, tc.rank, want)
		}
	}
}

// TestNearestRankClampsToARealElement covers both guards. Neither is reachable
// through EstimateHandoff at the fixed 0.95 quantile, which is exactly why
// they need holding: the day someone makes the quantile configurable, a rank
// off either end must land on a measurement rather than panic or wrap.
func TestNearestRankClampsToARealElement(t *testing.T) {
	sorted := etaSorted(10, 20, 30, 40)
	for _, quantile := range []float64{-1, -0.0001, 0} {
		if got := nearestRank(sorted, quantile); got != sorted[0] {
			t.Fatalf("quantile %v = %s, want the fastest measured %s", quantile, got, sorted[0])
		}
	}
	for _, quantile := range []float64{1, 1.0001, 2} {
		if got := nearestRank(sorted, quantile); got != sorted[len(sorted)-1] {
			t.Fatalf("quantile %v = %s, want the slowest measured %s", quantile, got, sorted[len(sorted)-1])
		}
	}
	single := etaSorted(17)
	for _, quantile := range []float64{0, HandoffQuantile, 1} {
		if got := nearestRank(single, quantile); got != single[0] {
			t.Fatalf("one sample at quantile %v = %s, want %s", quantile, got, single[0])
		}
	}
}

// TestOneSlowHandoffCannotDragTheETAButTwoCan is the practical shape of the
// ceiling rule, and the thing to know before reading an ETA that looks wrong.
// A single bad handoff in twenty is the twentieth of twenty and sits above the
// rank, so it moves nothing. The second one is what the waiting person feels.
func TestOneSlowHandoffCannotDragTheETA(t *testing.T) {
	now := time.Now()
	cohort := testCohort()

	fastOnly := make([]int, 20)
	for i := range fastOnly {
		fastOnly[i] = 10
	}
	quiet := mustEstimate(t, cohort, etaBounds(), completedSamples(cohort, now, fastOnly...), now)

	oneSlow := append([]int{300}, fastOnly[:19]...)
	withOne := mustEstimate(t, cohort, etaBounds(), completedSamples(cohort, now, oneSlow...), now)
	if withOne.Target != quiet.Target {
		t.Fatalf("one slow handoff moved the ETA from %s to %s", quiet.Target, withOne.Target)
	}
	if withOne.Samples != 20 {
		t.Fatalf("the slow handoff left the history: %d samples", withOne.Samples)
	}

	twoSlow := append([]int{300, 300}, fastOnly[:18]...)
	withTwo := mustEstimate(t, cohort, etaBounds(), completedSamples(cohort, now, twoSlow...), now)
	if withTwo.Target <= quiet.Target {
		t.Fatalf("two slow handoffs left the ETA at %s", withTwo.Target)
	}
}

// TestTheRankIsTakenOverMeasuredRowsOnly. Censored and stale rows are counted
// in the answer and must not be counted in the rank: a fixture with five
// abandoned requests would otherwise report a slower ETA than it delivers,
// from handoffs that never happened.
func TestTheRankIsTakenOverMeasuredRowsOnly(t *testing.T) {
	now := time.Now()
	cohort := testCohort()
	seconds := etaRun(20)
	clean := mustEstimate(t, cohort, etaBounds(), completedSamples(cohort, now, seconds...), now)

	mixed := completedSamples(cohort, now, seconds...)
	for i := 0; i < 5; i++ {
		mixed = append(mixed, YieldSample{
			Cohort:          cohort,
			RequestedAt:     now.Add(-2 * time.Hour),
			ExclusionReason: "waiter withdrew before the board went neutral",
		})
	}
	stale := now.Add(-MaxHandoffSampleAge - 2*time.Hour)
	mixed = append(mixed, YieldSample{Cohort: cohort, RequestedAt: stale, NeutralAt: stale.Add(9 * time.Minute)})

	got := mustEstimate(t, cohort, etaBounds(), mixed, now)
	if got.Target != clean.Target {
		t.Fatalf("censored and stale rows moved the ETA from %s to %s", clean.Target, got.Target)
	}
	if got.Samples != clean.Samples || got.Censored != 5 || got.Stale != 1 {
		t.Fatalf("counts: %d samples, %d censored, %d stale", got.Samples, got.Censored, got.Stale)
	}
}

// TestTheHistoryIsSortedBeforeTheRankIsTaken. Nothing promises the store hands
// the rows back in any order, and a rank read off an unsorted slice is an
// arbitrary sample rather than a quantile.
func TestTheHistoryIsSortedBeforeTheRankIsTaken(t *testing.T) {
	now := time.Now()
	cohort := testCohort()
	rising := etaRun(12)
	falling := make([]int, len(rising))
	for i, s := range rising {
		falling[len(rising)-1-i] = s
	}
	shuffled := []int{34, 10, 25, 13, 40, 19, 31, 16, 43, 22, 28, 37}

	want := mustEstimate(t, cohort, etaBounds(), completedSamples(cohort, now, rising...), now).Target
	for name, order := range map[string][]int{"falling": falling, "shuffled": shuffled} {
		got := mustEstimate(t, cohort, etaBounds(), completedSamples(cohort, now, order...), now).Target
		if got != want {
			t.Fatalf("%s order gave %s, want %s", name, got, want)
		}
	}
}

// TestSampleAgeIsNeverNegativeOrInvented covers both silent readings. An age
// is subtraction against a clock the caller supplies, so a sample stamped
// ahead of that clock is ordinary, and reporting it as a negative age in a
// line a person reads is the failure.
func TestSampleAgeIsNeverNegativeOrInvented(t *testing.T) {
	now := time.Now()
	for name, at := range map[string]time.Time{
		"no sample":        {},
		"stamped now":      now,
		"a second ahead":   now.Add(time.Second),
		"an hour ahead":    now.Add(time.Hour),
		"a whole day away": now.Add(24 * time.Hour),
	} {
		if got := sampleAge(now, at); got != 0 {
			t.Fatalf("%s: age = %s, want 0", name, got)
		}
	}
	if got := sampleAge(now, now.Add(-90*time.Minute)); got != 90*time.Minute {
		t.Fatalf("age = %s, want 1h30m0s", got)
	}
}

// TestSampleAgeRoundsToTheSecond. The line is read by a person, so sub-second
// precision is noise; rounding is to the nearest second in both directions.
func TestSampleAgeRoundsToTheSecond(t *testing.T) {
	now := time.Now()
	for _, tc := range []struct {
		ago  time.Duration
		want time.Duration
	}{
		{1400 * time.Millisecond, time.Second},
		{1500 * time.Millisecond, 2 * time.Second},
		{2600 * time.Millisecond, 3 * time.Second},
		{400 * time.Millisecond, 0},
	} {
		if got := sampleAge(now, now.Add(-tc.ago)); got != tc.want {
			t.Fatalf("%s ago reads as %s, want %s", tc.ago, got, tc.want)
		}
	}
}

// TestProvenanceShowsNoNegativeAge is the same fact one level up, at the
// sentence. An estimate whose timestamps are missing still has to produce a
// line, and "0s old" is the honest reading of a sample that is not there.
func TestProvenanceShowsNoNegativeAge(t *testing.T) {
	now := time.Now()
	line := HandoffEstimate{
		Cohort:   testCohort(),
		Target:   40 * time.Second,
		Source:   HandoffFromHistory,
		Samples:  6,
		Quantile: HandoffQuantile,
		Margin:   HandoffMargin,
	}.Provenance(now)
	if !strings.Contains(line, "newest 0s old, oldest 0s old") {
		t.Fatalf("provenance with no timestamps: %q", line)
	}
	ahead := HandoffEstimate{
		Cohort:       testCohort(),
		Target:       40 * time.Second,
		Source:       HandoffFromHistory,
		Samples:      6,
		Quantile:     HandoffQuantile,
		Margin:       HandoffMargin,
		OldestSample: now.Add(time.Hour),
		NewestSample: now.Add(2 * time.Hour),
	}.Provenance(now)
	if !strings.Contains(ahead, "newest 0s old, oldest 0s old") {
		t.Fatalf("provenance for samples ahead of the clock: %q", ahead)
	}
}

// TestProvenanceCountsWhatItLeftOut. The counts are the reader's only way to
// tell a confident ETA from one resting on two measurements and nine
// abandoned requests, so each is named and none is folded into another.
func TestProvenanceCountsWhatItLeftOut(t *testing.T) {
	now := time.Now()
	cohort := testCohort()
	samples := completedSamples(cohort, now, etaRun(20)...)
	for i := range samples[:3] {
		samples[i].SafetyOverrun = true
	}
	for i := 0; i < 2; i++ {
		samples = append(samples, YieldSample{
			Cohort:          cohort,
			RequestedAt:     now.Add(-3 * time.Hour),
			ExclusionReason: "board quarantined before it reached neutral",
		})
	}
	// Staleness is judged on NeutralAt, so this row has to reach neutral past
	// the age bound, not merely have been requested before it.
	stale := now.Add(-MaxHandoffSampleAge - 2*time.Hour)
	samples = append(samples, YieldSample{Cohort: cohort, RequestedAt: stale, NeutralAt: stale.Add(time.Minute)})

	estimate := mustEstimate(t, cohort, etaBounds(), samples, now)
	line := estimate.Provenance(now)
	for _, want := range []string{"p95", "20 samples", "2 censored", "1 stale", "3 safety overruns", HandoffMargin.String() + " margin"} {
		if !strings.Contains(line, want) {
			t.Fatalf("provenance %q omits %q", line, want)
		}
	}
	if estimate.Overruns != 3 {
		t.Fatalf("overruns = %d, want 3", estimate.Overruns)
	}
}

// TestASlowerCohortDoesNotLendItsRank. The rank is per cohort by construction,
// and this is the reading that matters: a neighbouring fixture's slow history
// is not evidence about this one, however comparable it looks.
func TestASlowerCohortDoesNotLendItsRank(t *testing.T) {
	now := time.Now()
	cohort := testCohort()
	other := testCohort()
	other.FixtureRevision = "fixture-d"

	mine := completedSamples(cohort, now, etaRun(20)...)
	alone := mustEstimate(t, cohort, etaBounds(), mine, now)

	slow := make([]int, 20)
	for i := range slow {
		slow[i] = 600
	}
	together := mustEstimate(t, cohort, etaBounds(), append(mine, completedSamples(other, now, slow...)...), now)
	if together.Target != alone.Target || together.Samples != alone.Samples {
		t.Fatalf("the other fixture's history reached this ETA: %s over %d samples, want %s over %d",
			together.Target, together.Samples, alone.Target, alone.Samples)
	}
}

// TestStalenessIsJudgedOnWhenTheBoardWentNeutral is what the first draft of
// this file got wrong, and the arithmetic is worth holding. The age bound is
// measured from NeutralAt, not from RequestedAt, so a handoff REQUESTED five
// weeks ago that finished yesterday is recent evidence and counts; the cut is
// exact, and a sample landing precisely on the bound is still measured.
func TestStalenessIsJudgedOnWhenTheBoardWentNeutral(t *testing.T) {
	now := time.Now()
	cohort := testCohort()
	base := completedSamples(cohort, now, etaRun(20)...)

	onTheBound := YieldSample{
		Cohort:      cohort,
		RequestedAt: now.Add(-MaxHandoffSampleAge - time.Hour),
		NeutralAt:   now.Add(-MaxHandoffSampleAge),
	}
	kept := mustEstimate(t, cohort, etaBounds(), append(append([]YieldSample{}, base...), onTheBound), now)
	if kept.Samples != 21 || kept.Stale != 0 {
		t.Fatalf("a sample exactly on the age bound: %d samples, %d stale, want 21 and 0", kept.Samples, kept.Stale)
	}

	justPast := onTheBound
	justPast.NeutralAt = now.Add(-MaxHandoffSampleAge - time.Nanosecond)
	dropped := mustEstimate(t, cohort, etaBounds(), append(append([]YieldSample{}, base...), justPast), now)
	if dropped.Samples != 20 || dropped.Stale != 1 {
		t.Fatalf("one nanosecond past the bound: %d samples, %d stale, want 20 and 1", dropped.Samples, dropped.Stale)
	}
}
