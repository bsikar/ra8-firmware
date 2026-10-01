package board

// One rule, three readers: a yield cohort must name the board it is about.
//
// A cohort decides which history a handoff is estimated over AND which history
// the completed measurement joins, because the cohort recorded on the lease is
// the bucket YieldSampleFor files the sample against. A cohort naming another
// board therefore does damage twice: it quotes a stranger's latency to the
// requester now, and it writes this board's measurement into that stranger's
// history later, where nothing downstream can tell it apart from work that
// board actually did.
//
// PlanYield has refused it from the start. Apply, which is the path that
// actually writes the value onto the lease, did not, and a plan is advisory by
// its own doc comment: RequestYield can be issued without one. So the rule
// lives here, and the two admission sites and the snapshot invariant all ask
// it in the same words.
func checkCohortNamesBoard(cohort YieldCohort, boardID string) error {
	if cohort.BoardID == boardID {
		return nil
	}
	return &Error{InvalidArgument,
		"yield cohort names board " + cohort.BoardID + ", not the board being asked to yield"}
}
