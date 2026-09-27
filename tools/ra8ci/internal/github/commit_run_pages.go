// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

// RunsOn walks a commit's workflow runs a page at a time, and the decision to
// stop was the listing's own arithmetic: read a page, stop once total_count is
// no larger than the rows read so far.
//
// That is the arithmetic the check run walk already abandoned, for a reason
// that holds here word for word. total_count is answered per request, from
// whatever the listing looked like at that moment, and a commit gains workflow
// runs while it is being paged: a re-run is dispatched, a labeller fires on the
// same head, a workflow_run trigger starts a second workflow. A count that
// comes back smaller than the rows already served ends the walk on a full
// page, and every run behind it goes unread with nothing marking the listing
// short.
//
// What the short read costs here is the evidence. SelectEvidenceRun refuses a
// commit carrying two decided runs of the named workflow, because a re-run's
// answer and the original's disagree exactly when the comparison matters most.
// That refusal is only as good as the listing it judges: drop the second
// decided run on an unread page and the selection is no longer ambiguous, it is
// confident and wrong, and the run ID that goes into the comparison document is
// the one nobody would have chosen. The same dropped run turns a commit CI did
// cover into ErrEvidenceRunNotFound, which an operator reads as a commit the
// workflow never ran on.
//
// So the walk ends on a page the API could not fill, and the count is kept only
// as the second half of the same question. A full page always costs a
// confirming request, whatever the count says; a short page still defers to a
// count that says there is more, which is the shape the listing takes while
// runs are being added underneath it. A listing whose every page up to the
// ceiling comes back full is refused rather than reported: a full last page and
// a listing that continues are the same page, and reporting one as the other is
// the partial answer this rule exists to prevent.
const commitRunPageSize = 100

// commitRunPageEndsTheWalk reports whether the page just read is the last one
// the commit has to offer.
//
// An empty page ends it outright: there is nothing on it to argue with and
// nothing beyond it to fetch, and a count still claiming more at that point is
// describing runs the API is not serving.
func commitRunPageEndsTheWalk(rows, stated, page int) bool {
	if rows <= 0 {
		return true
	}
	if rows >= commitRunPageSize {
		return false
	}
	return stated <= page*commitRunPageSize
}
