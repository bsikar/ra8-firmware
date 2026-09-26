package board

// The plan's cohort must be the cohort the promise was made under.
//
// A yield plan is two answers in one value: the number the requester is held
// to, and the account of where that number came from. For an outstanding
// request those two come from different places. promised() pins the target to
// the one recorded on the lease, because a waiter was already given it, while
// the estimate around it (cohort, sample counts, ages, provenance line) is
// computed over the cohort the CALLER derived from held work at poll time.
// Nothing asked whether the two cohorts were the same one.
//
// checkCohortNamesBoard is not that rule and cannot stand in for it: it holds
// the cohort to the board, so two cohorts for the same board differing in
// fixture revision, task name, catalog digest or image digest both pass it.
// Those are exactly the fields YieldCohort's own doc comment says start a
// fresh history, "a fixture revision or image change starts a fresh history
// instead of averaging across the change".
//
// The cost is a plan that reads as one answer and is two. Explain() quotes the
// caller's cohort and its sample count beside a target estimated over the
// lease's, so the requester is told a promise rests on history that never
// produced it; Overdue() then judges the handoff against a bound from one
// cohort while the numbers offered as its justification come from another.
// The recorded cohort is the one the shown target was estimated over, which is
// why the lease retains it at all.
//
// The refusal is loud and in YieldSampleFor's words, because that is the same
// disagreement caught at the other end of the same handoff: one of the two
// cohorts is wrong, and a plan that silently adopted either would decide which
// history this handoff belongs to without telling anyone it had a choice. A
// plan is refused rather than corrected; a caller whose derivation disagrees
// with the lease has a bug the quiet path would hide.
func checkPlanCohortIsThePromisedOne(cohort YieldCohort, lease *Lease) error {
	if lease == nil || lease.HandoffCohort == (YieldCohort{}) {
		return nil
	}
	if cohort == lease.HandoffCohort {
		return nil
	}
	return &Error{InvalidArgument, "yield cohort contradicts the cohort recorded on the lease"}
}
