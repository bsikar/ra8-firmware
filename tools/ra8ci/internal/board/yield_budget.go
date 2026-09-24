package board

import (
	"fmt"
	"math"
	"slices"
	"time"
)

// Handoff estimation: the dynamic yield budget.
//
// Three clocks stay distinct and this file estimates exactly one of them: the
// time from a human's yield request to a neutral board. It is NOT a HIL test's
// validity deadline and it is NOT the safety limit of an indivisible flash,
// erase, verify, or recovery phase. The estimate is an ETA shown to the person
// waiting; it never authorizes interrupting an unsafe phase and it never
// shortens a bound the task declared for itself.
const (
	// DefaultHandoffTarget is the handoff target and the default process-stop
	// grace for a separately declared cancel-safe experiment. It is never a
	// safety bound, and deliberately never a fallback here: a task that
	// declares no bounds has an unknown ETA and EstimateHandoff refuses it
	// rather than quietly answering thirty seconds.
	DefaultHandoffTarget = 30 * time.Second

	// MinimumHandoffSamples is the comparable-history floor below which the
	// declared bounds are used unchanged. It matches the HIL estimator's floor
	// so the two do not disagree about what "enough history" means.
	MinimumHandoffSamples = 5

	// MaxHandoffSamples bounds one estimation pass.
	MaxHandoffSamples = 10000

	// HandoffQuantile is the conservative high quantile taken over comparable
	// request-to-neutral latencies, by nearest rank (never interpolated: an
	// interpolated quantile invents a latency nobody measured).
	HandoffQuantile = 0.95

	// HandoffMargin is added to the quantile. It covers the sampling gap
	// between the last observed handoff and the next one, not clock skew,
	// which DeadlineFence handles separately.
	HandoffMargin = 5 * time.Second

	// MaxHandoffBound caps a declared bound and the resulting estimate.
	MaxHandoffBound = time.Hour

	// MaxHandoffSampleAge drops history too old to describe today's fixture.
	MaxHandoffSampleAge = 30 * 24 * time.Hour

	maxCohortFieldBytes  = 256
	maxExclusionBytes    = 256
	maxCohortFieldsCount = 6
)

// YieldCohort identifies a genuinely comparable request-to-neutral history.
// Every field is supplied by trusted catalog, inventory, and lease state, not
// by a holder's self-report. Comparison is exact: a cohort that differs in one
// field is a different cohort, so a fixture revision or image change starts a
// fresh history instead of averaging across the change.
type YieldCohort struct {
	BoardID         string
	BoardModel      string
	FixtureRevision string
	TaskName        string
	CatalogDigest   string
	// ImageSHA256 may be empty for a task that flashes no image. Empty is
	// still part of the identity: an imageless task never borrows an imaged
	// task's history.
	ImageSHA256 string
}

// ValidateYieldCohort refuses a cohort that cannot identify comparable work.
func ValidateYieldCohort(cohort YieldCohort) error {
	required := []struct {
		name  string
		value string
	}{
		{"board ID", cohort.BoardID},
		{"board model", cohort.BoardModel},
		{"fixture revision", cohort.FixtureRevision},
		{"task name", cohort.TaskName},
		{"catalog digest", cohort.CatalogDigest},
	}
	for _, field := range required {
		if field.value == "" {
			return &Error{InvalidArgument, "yield cohort is missing its " + field.name}
		}
	}
	all := append(required, struct {
		name  string
		value string
	}{"image digest", cohort.ImageSHA256})
	for _, field := range all {
		if len(field.value) > maxCohortFieldBytes {
			return &Error{InvalidArgument, "yield cohort " + field.name + " is too long"}
		}
	}
	return nil
}

// YieldSample is one recorded yield request and its outcome. A request that
// failed, was cancelled, or never reached a neutral board is retained with an
// ExclusionReason: it stays in the history as evidence, and it is never passed
// off as a completed latency measurement.
type YieldSample struct {
	Cohort      YieldCohort
	RequestedAt time.Time
	NeutralAt   time.Time
	// SafetyOverrun records that the holder finished an indivisible phase past
	// the target before yielding. Such a sample is still a real completed
	// handoff and stays in the estimate; dropping it would bias the ETA low in
	// exactly the cohort that overruns.
	SafetyOverrun bool
	// ExclusionReason is empty for a completed handoff. Nonempty means the row
	// is retained but censored.
	ExclusionReason string
}

// Completed reports whether the sample carries a usable latency.
func (s YieldSample) Completed() bool {
	return s.ExclusionReason == "" && s.NeutralAt.After(s.RequestedAt)
}

// Latency is the request-to-neutral time; zero for a censored sample.
func (s YieldSample) Latency() time.Duration {
	if !s.Completed() {
		return 0
	}
	return s.NeutralAt.Sub(s.RequestedAt)
}

// DeclaredHandoffBounds are the task's own declared maximum safe-step and
// restore/probe durations. A board task must declare both; without them its
// ETA is unknown and automatic dispatch is rejected.
type DeclaredHandoffBounds struct {
	SafeStepBound     time.Duration
	RestoreProbeBound time.Duration
}

// SafetyBound is the declared request-to-neutral worst case: the longest
// indivisible step plus the restore and probe that must follow it.
func (b DeclaredHandoffBounds) SafetyBound() time.Duration {
	return b.SafeStepBound + b.RestoreProbeBound
}

// ValidateHandoffBounds refuses undeclared or unusable bounds.
func ValidateHandoffBounds(bounds DeclaredHandoffBounds) error {
	if bounds.SafeStepBound <= 0 || bounds.RestoreProbeBound <= 0 {
		return &Error{InvalidArgument, "board task declares no safe-step and restore bounds, so its handoff ETA is unknown"}
	}
	if bounds.SafeStepBound > MaxHandoffBound || bounds.RestoreProbeBound > MaxHandoffBound ||
		bounds.SafetyBound() > MaxHandoffBound {
		return &Error{InvalidArgument, "declared handoff bounds exceed the estimator's maximum"}
	}
	return nil
}

// HandoffSource names where an estimate's number came from.
type HandoffSource string

const (
	HandoffFromHistory        HandoffSource = "history"
	HandoffFromDeclaredBounds HandoffSource = "declared"
)

// HandoffEstimate is the visible dynamic handoff estimate. Cohort, sample
// count, and sample age are part of the answer, not diagnostics: a requester
// told "about 40 seconds" deserves to see whether that rests on twelve recent
// measurements or on nothing at all.
type HandoffEstimate struct {
	Cohort       YieldCohort
	Target       time.Duration
	Source       HandoffSource
	Samples      int
	Censored     int
	Stale        int
	Overruns     int
	Quantile     float64
	Margin       time.Duration
	SafetyBound  time.Duration
	OldestSample time.Time
	NewestSample time.Time
}

// EstimateHandoff produces the dynamic yield budget for one cohort.
//
// With fewer than MinimumHandoffSamples comparable completed handoffs the
// declared bounds are used unchanged. With enough history the estimate is the
// HandoffQuantile nearest-rank latency plus HandoffMargin, and is then raised
// to the declared safety bound if it fell below it: the estimator may lengthen
// a task's expected handoff, never shorten the bound the task declared for its
// own indivisible work. Samples from another cohort are ignored, censored rows
// are counted but never measured, and a row that claims completion without a
// usable pair of timestamps is refused outright rather than silently dropped.
func EstimateHandoff(cohort YieldCohort, bounds DeclaredHandoffBounds, samples []YieldSample, now time.Time) (HandoffEstimate, error) {
	if err := ValidateYieldCohort(cohort); err != nil {
		return HandoffEstimate{}, err
	}
	if err := ValidateHandoffBounds(bounds); err != nil {
		return HandoffEstimate{}, err
	}
	if now.IsZero() {
		return HandoffEstimate{}, &Error{InvalidArgument, "handoff estimate needs the current time"}
	}
	if len(samples) > MaxHandoffSamples {
		return HandoffEstimate{}, &Error{InvalidArgument, "yield history exceeds the sample bound"}
	}

	declared := bounds.SafetyBound()
	estimate := HandoffEstimate{
		Cohort:      cohort,
		Target:      declared,
		Source:      HandoffFromDeclaredBounds,
		Quantile:    HandoffQuantile,
		SafetyBound: declared,
	}

	latencies := make([]time.Duration, 0, len(samples))
	for _, sample := range samples {
		if sample.Cohort != cohort {
			continue
		}
		if err := validateYieldSample(sample, now); err != nil {
			return HandoffEstimate{}, err
		}
		if sample.ExclusionReason != "" {
			estimate.Censored++
			continue
		}
		if now.Sub(sample.NeutralAt) > MaxHandoffSampleAge {
			estimate.Stale++
			continue
		}
		latency := sample.Latency()
		if latency > MaxHandoffBound {
			latency = MaxHandoffBound
		}
		latencies = append(latencies, latency)
		if sample.SafetyOverrun {
			estimate.Overruns++
		}
		if estimate.OldestSample.IsZero() || sample.NeutralAt.Before(estimate.OldestSample) {
			estimate.OldestSample = sample.NeutralAt
		}
		if sample.NeutralAt.After(estimate.NewestSample) {
			estimate.NewestSample = sample.NeutralAt
		}
	}
	estimate.Samples = len(latencies)
	if estimate.Samples < MinimumHandoffSamples {
		return estimate, nil
	}

	slices.Sort(latencies)
	target := nearestRank(latencies, HandoffQuantile) + HandoffMargin
	if target > MaxHandoffBound {
		target = MaxHandoffBound
	}
	if target < declared {
		target = declared
	}
	estimate.Source = HandoffFromHistory
	estimate.Margin = HandoffMargin
	estimate.Target = target
	return estimate, nil
}

// Provenance is the one-line explanation shown beside the ETA.
func (e HandoffEstimate) Provenance(now time.Time) string {
	cohort := fmt.Sprintf("cohort board=%s fixture=%s task=%s", e.Cohort.BoardID, e.Cohort.FixtureRevision, e.Cohort.TaskName)
	if e.Source != HandoffFromHistory {
		return fmt.Sprintf("%s from declared bounds (%d comparable samples, %d censored, %d stale); %s",
			e.Target, e.Samples, e.Censored, e.Stale, cohort)
	}
	return fmt.Sprintf("%s from p%d of %d samples plus %s margin (%d censored, %d stale, %d safety overruns); newest %s old, oldest %s old; %s",
		e.Target, int(math.Round(e.Quantile*100)), e.Samples, e.Margin, e.Censored, e.Stale, e.Overruns,
		sampleAge(now, e.NewestSample), sampleAge(now, e.OldestSample), cohort)
}

func sampleAge(now, at time.Time) time.Duration {
	if at.IsZero() || !now.After(at) {
		return 0
	}
	return now.Sub(at).Round(time.Second)
}

// validateYieldSample fails closed on a row that claims a completed handoff it
// cannot support. A censored row is exempt: a request that never reached a
// neutral board legitimately has no neutral time, and refusing it would push
// the recorder toward inventing one.
func validateYieldSample(sample YieldSample, now time.Time) error {
	if len(sample.ExclusionReason) > maxExclusionBytes {
		return &Error{InvalidArgument, "yield sample exclusion reason is too long"}
	}
	if sample.ExclusionReason != "" {
		return nil
	}
	if sample.RequestedAt.IsZero() || sample.NeutralAt.IsZero() {
		return &Error{InvalidArgument, "yield sample claims a completed handoff without both times"}
	}
	if !sample.NeutralAt.After(sample.RequestedAt) {
		return &Error{InvalidArgument, "yield sample reaches neutral before its request"}
	}
	if sample.NeutralAt.After(now.Add(MaxClockOffset)) {
		return &Error{InvalidArgument, "yield sample reaches neutral in the future"}
	}
	return nil
}

func nearestRank(sorted []time.Duration, quantile float64) time.Duration {
	rank := int(math.Ceil(quantile * float64(len(sorted))))
	if rank < 1 {
		rank = 1
	}
	if rank > len(sorted) {
		rank = len(sorted)
	}
	return sorted[rank-1]
}
