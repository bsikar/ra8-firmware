// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

// PublishedRuns reads a commit's check runs a page at a time and decides when
// it has them all. The decision was the listing's own arithmetic: read a page,
// and stop once total_count is no larger than the pages read so far.
//
// That trusts a count to describe rows it does not travel with. The check runs
// on a commit change while they are being paged, a run posted between two
// requests moves every row after it, and total_count is answered per request
// from whatever the listing looked like at that moment. A count that comes
// back smaller than the rows actually there ends the walk early, and nothing
// downstream can tell: PublishedCheckRuns carries no mark for a listing that
// was cut short, so a partial answer is read as the whole commit.
//
// What a short read costs is the reconciliation itself. ReconcilePublish
// answers PublishNeeded when nothing in the listing carries the intended
// name, so a run sitting on a page that was never read is a run the caller
// posts a second time, under a name branch protection may require, which is
// the blind repeat the implementation contract exists to forbid.
// UnplannedRuns is the mirror: a stale run on an unread page is one nothing
// reports, and the commit is described as settled.
//
// So the walk ends on a page the API could not fill, the way the workflow run
// walk does, and the count is kept only as the second half of the same
// question. A full page always costs a confirming request, whatever the count
// says; a short page still defers to a count that says there is more, which is
// the shape the listing takes while runs are being added underneath it.
const publishedCheckRunPageSize = 100

// commitCheckRunPageEndsTheWalk reports whether the page just read is the last
// one the commit has to offer.
//
// An empty page ends it outright: there is nothing on it to argue with and
// nothing beyond it to fetch, and a count still claiming more at that point is
// describing runs the API is not serving.
func commitCheckRunPageEndsTheWalk(rows, stated, page int) bool {
	if rows <= 0 {
		return true
	}
	if rows >= publishedCheckRunPageSize {
		return false
	}
	return stated <= page*publishedCheckRunPageSize
}
