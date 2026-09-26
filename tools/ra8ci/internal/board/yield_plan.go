package board

import "time"

// Planning a yield: what the requester is shown before the board is asked.
//
// EstimateHandoff answers "how long should this take" and YieldSampleFor
// records what it actually took. Neither is reachable from a yield request:
// nothing in the state machine consults an estimate, and a requester is told
// only that a yield was asked for. This file is the seam between the two. It
// decides whether the yield may be dispatched at all, anchors the ETA to the
// request rather than to the poll, and carries the shown target back out so
// the sample recorded later is measured against the number the requester
// actually saw.

// YieldDispatch names who is asking, because the two are held to different
// standards. A person may ask for a board back knowing the ETA is unknown; a
// scheduler may not, since nothing downstream would ever be told the wait is
// unbounded.
type YieldDispatch string

const (
	// YieldAutomatic is a yield the scheduler raises on a queued waiter's
	// behalf. It is rejected when the task declares no handoff bounds.
	YieldAutomatic YieldDispatch = "automatic"

	// YieldOperator is a yield an authenticated person asked for. It proceeds
	// with an unknown ETA rather than being refused.
	YieldOperator YieldDispatch = "operator"
)

// HandoffUnknown is the source of a plan with no ETA: the task declared no
// safe-step and restore bounds and has no history to stand in for them.
// EstimateHandoff never returns it, since it refuses such a cohort outright.
const HandoffUnknown HandoffSource = "unknown"

// HandoffAsPromised is the source of a target that was already shown to a
// requester and recorded on the lease. It outranks a fresh estimate for as
// long as the request is outstanding: more history arriving mid-handoff is a
// reason to learn, never a reason to move a deadline somebody was already
// given. EstimateHandoff never returns it either; only a recorded request
// does.
const HandoffAsPromised HandoffSource = "promised"

// YieldPlan is the answer to "may this yield be asked for, and what do I tell
// the requester". It is advisory: Apply still enforces the transition, and a
// plan never authorizes interrupting an indivisible phase.
type YieldPlan struct {
	Estimate HandoffEstimate
	Dispatch YieldDispatch

	// RequestedAt anchors the ETA. For a yield already outstanding it is the
	// moment the board was first asked, NOT now: a requester refreshing the
	// page must not watch the deadline slide away from them once per poll.
	RequestedAt time.Time

	// Outstanding reports that the board was already asked to yield, so this
	// plan describes a handoff in flight rather than one about to start.
	Outstanding bool

	// ExpectedNeutralAt is RequestedAt plus the estimate, or zero when the
	// ETA is unknown. Zero is the honest answer and is never collapsed to
	// RequestedAt, which would read as "neutral already".
	ExpectedNeutralAt time.Time
}

// Known reports whether the plan carries an ETA at all.
func (p YieldPlan) Known() bool {
	return p.Estimate.Source != HandoffUnknown && p.Estimate.Target > 0
}

// ShownTarget is the estimate the requester was shown, to be passed to
// YieldSampleFor so the recorded sample is judged against the number that was
// actually promised rather than against whatever the estimator would say by
// the time the handoff completes.
func (p YieldPlan) ShownTarget() time.Duration {
	if !p.Known() {
		return 0
	}
	return p.Estimate.Target
}

// Overdue reports that an outstanding handoff has passed the target shown to
// the requester. It is not a licence to preempt: the holder still finishes its
// indivisible phase. It is the trigger for reporting the overrun and a revised
// ETA, and it is always false when no ETA was shown.
func (p YieldPlan) Overdue(now time.Time) bool {
	return p.Known() && p.Outstanding && now.After(p.ExpectedNeutralAt)
}

// Explain is the one line shown beside the ETA.
func (p YieldPlan) Explain(now time.Time) string {
	if !p.Known() {
		return "handoff ETA unknown: the board task declares no safe-step and restore bounds; " +
			"cohort board=" + p.Estimate.Cohort.BoardID +
			" fixture=" + p.Estimate.Cohort.FixtureRevision +
			" task=" + p.Estimate.Cohort.TaskName
	}
	return p.Estimate.Provenance(now)
}

// PlanYield decides whether waiterID may ask this board to yield and what the
// requester is told.
//
// Admission is the same check Apply makes for RequestYield, deliberately so: a
// plan that promised an ETA for a yield the state machine would refuse would
// be worse than no plan. With declared bounds the ETA comes from
// EstimateHandoff over comparable history. Without them an automatic dispatch
// is rejected, and an operator dispatch proceeds with the ETA reported as
// unknown.
//
// The cohort must name the board in the snapshot. It arrives from a caller
// that derived it separately (the server reads it from held work), so this is
// the place the two derivations are made to agree: a cohort for another board
// would estimate this handoff over another board's history and, worse, be
// recorded on this lease and file the completed measurement there too. Loud
// beats quiet here, the same way the history read refuses a row outside the
// cohort it asked for rather than skipping it.
func PlanYield(s Snapshot, waiterID string, dispatch YieldDispatch, cohort YieldCohort, bounds DeclaredHandoffBounds, samples []YieldSample, now time.Time) (YieldPlan, error) {
	if dispatch != YieldAutomatic && dispatch != YieldOperator {
		return YieldPlan{}, &Error{InvalidArgument, "unknown yield dispatch"}
	}
	if now.IsZero() {
		return YieldPlan{}, &Error{InvalidArgument, "yield plan needs the current time"}
	}
	if err := Validate(s); err != nil {
		return YieldPlan{}, err
	}
	if err := ValidateYieldCohort(cohort); err != nil {
		return YieldPlan{}, err
	}
	if err := checkCohortNamesBoard(cohort, s.BoardID); err != nil {
		return YieldPlan{}, err
	}
	if err := admitYield(s, waiterID); err != nil {
		return YieldPlan{}, err
	}

	plan := YieldPlan{Dispatch: dispatch, RequestedAt: now}
	if !s.Lease.YieldRequestedAt.IsZero() {
		plan.Outstanding = true
		plan.RequestedAt = s.Lease.YieldRequestedAt
	}

	if err := ValidateHandoffBounds(bounds); err != nil {
		if dispatch == YieldAutomatic {
			return YieldPlan{}, err
		}
		plan.Estimate = HandoffEstimate{Cohort: cohort, Source: HandoffUnknown}
		return promised(plan, s), nil
	}

	estimate, err := EstimateHandoff(cohort, bounds, samples, now)
	if err != nil {
		return YieldPlan{}, err
	}
	plan.Estimate = estimate
	plan.ExpectedNeutralAt = plan.RequestedAt.Add(estimate.Target)
	return promised(plan, s), nil
}

// promised replaces a freshly estimated target with the one already recorded
// on the lease, for an outstanding request that carries one.
//
// The estimate around it is kept as it stands, cohort, sample counts and all,
// because that is still the honest account of what the history says now. Only
// the number the requester is held to is pinned, which is the whole point of
// recording it: a handoff is not overdue against an estimate that moved after
// the promise was made.
func promised(plan YieldPlan, s Snapshot) YieldPlan {
	if !plan.Outstanding || s.Lease == nil || s.Lease.HandoffTarget <= 0 {
		return plan
	}
	plan.Estimate.Target = s.Lease.HandoffTarget
	plan.Estimate.Source = HandoffAsPromised
	plan.ExpectedNeutralAt = plan.RequestedAt.Add(s.Lease.HandoffTarget)
	return plan
}
