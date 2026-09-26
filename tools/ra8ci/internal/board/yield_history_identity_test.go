package board

import (
	"strings"
	"testing"
	"time"
)

// One lease leaves at most one yield sample, which YieldSample's doc comment
// states and the store enforces. These hold the estimator to it, since a
// duplicated measurement is weighted twice in a quantile taken over a small
// sample floor and is reported as history that does not exist.

func identifiedSamples(cohort YieldCohort, now time.Time, seconds ...int) []YieldSample {
	samples := completedSamples(cohort, now, seconds...)
	for i := range samples {
		samples[i].LeaseID = "lease-" + string(rune('a'+i))
		samples[i].WaiterID = "waiter-" + string(rune('a'+i))
	}
	return samples
}

func TestEstimateRefusesAHistoryThatMeasuresOneLeaseTwice(t *testing.T) {
	now := time.Date(2026, 9, 24, 18, 0, 0, 0, time.UTC)
	cohort, bounds := testCohort(), testBounds()
	samples := identifiedSamples(cohort, now, 30, 34, 36, 40, 44, 90)
	if _, err := EstimateHandoff(cohort, bounds, samples, now); err != nil {
		t.Fatalf("distinct history refused: %v", err)
	}

	samples[4].LeaseID = samples[0].LeaseID
	_, err := EstimateHandoff(cohort, bounds, samples, now)
	if !IsCode(err, InvalidArgument) {
		t.Fatalf("history measuring one lease twice accepted: %v", err)
	}
	if !strings.Contains(err.Error(), samples[0].LeaseID) {
		t.Fatalf("refusal does not name the lease: %v", err)
	}
}

// The plan is the reader an operator actually sees, so the refusal has to
// reach it rather than stopping at the estimator.
func TestPlanYieldRefusesAHistoryThatMeasuresOneLeaseTwice(t *testing.T) {
	s, waiter, asked := plannableBoard(t)
	now := asked.Add(2 * time.Second)
	cohort := testCohort()
	samples := identifiedSamples(cohort, now, 30, 34, 36, 40, 44, 90)
	samples[2].LeaseID = samples[1].LeaseID

	if _, err := PlanYield(s, waiter, YieldAutomatic, cohort, testBounds(), samples, now); !IsCode(err, InvalidArgument) {
		t.Fatalf("plan built over a doubled measurement: %v", err)
	}
}

// Censored rows carry no latency and move no quantile. A board that failed to
// hand off five times running is five pieces of evidence, and the existing
// floor test depends on counting repeats of one, so they stay exempt.
func TestCensoredRowsMayRepeatTheSameLease(t *testing.T) {
	now := time.Date(2026, 9, 24, 18, 0, 0, 0, time.UTC)
	cohort := testCohort()
	history := make([]YieldSample, 0, MinimumHandoffSamples+1)
	for i := 0; i <= MinimumHandoffSamples; i++ {
		history = append(history, YieldSample{
			Cohort: cohort, LeaseID: "lease-stuck", WaiterID: "waiter-stuck",
			RequestedAt: now.Add(-time.Duration(i+1) * time.Hour), ExclusionReason: YieldExcludedExpired,
		})
	}

	estimate, err := EstimateHandoff(cohort, testBounds(), history, now)
	if err != nil {
		t.Fatalf("repeated censored evidence refused: %v", err)
	}
	if estimate.Censored != len(history) || estimate.Samples != 0 {
		t.Fatalf("censored rows were measured: %#v", estimate)
	}
}

// A measured row and a censored one for the same lease is a history that says
// the handoff both completed and did not. The measured one is what the
// estimate would use, so it is the one held to the rule.
func TestACensoredRowDoesNotShieldADuplicatedMeasurement(t *testing.T) {
	now := time.Date(2026, 9, 24, 18, 0, 0, 0, time.UTC)
	cohort, bounds := testCohort(), testBounds()
	samples := identifiedSamples(cohort, now, 30, 34, 36, 40, 44, 90)
	censored := YieldSample{
		Cohort: cohort, LeaseID: samples[3].LeaseID, WaiterID: samples[3].WaiterID,
		RequestedAt: now.Add(-9 * time.Hour), ExclusionReason: YieldExcludedWithdrawn,
	}

	if _, err := EstimateHandoff(cohort, bounds, append(samples, censored), now); err != nil {
		t.Fatalf("one measurement beside one censored row refused: %v", err)
	}

	doubled := append(identifiedSamples(cohort, now, 30, 34, 36, 40, 44, 90), samples[3])
	if _, err := EstimateHandoff(cohort, bounds, doubled, now); !IsCode(err, InvalidArgument) {
		t.Fatalf("a censored row beside it hid the duplicate: %v", err)
	}
}

// Rows from another cohort are ignored by contract, so a lease appearing there
// as well is not this estimate's business.
func TestAnotherCohortsRowIsNotADuplicate(t *testing.T) {
	now := time.Date(2026, 9, 24, 18, 0, 0, 0, time.UTC)
	cohort, bounds := testCohort(), testBounds()
	samples := identifiedSamples(cohort, now, 30, 34, 36, 40, 44, 90)
	elsewhere := samples[0]
	elsewhere.Cohort.BoardID = "board-elsewhere"

	if _, err := EstimateHandoff(cohort, bounds, append(samples, elsewhere), now); err != nil {
		t.Fatalf("a row outside the cohort was counted as a duplicate: %v", err)
	}
}

// Identity-less rows are the store's refusal, not this one, and they collide
// with nothing.
func TestRowsWithoutALeaseIDAreLeftAlone(t *testing.T) {
	now := time.Date(2026, 9, 24, 18, 0, 0, 0, time.UTC)
	cohort, bounds := testCohort(), testBounds()

	estimate, err := EstimateHandoff(cohort, bounds, completedSamples(cohort, now, 30, 34, 36, 40, 44, 90), now)
	if err != nil {
		t.Fatalf("anonymous history refused: %v", err)
	}
	if estimate.Samples != 6 || estimate.Source != HandoffFromHistory {
		t.Fatalf("anonymous history stopped being estimated over: %#v", estimate)
	}
}
