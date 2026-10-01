package store

import (
	"fmt"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
)

// Holding a stored yield sample to the clock the history was read against.
//
// The read bounds its scan on one side only: yieldHistoryCutoff drops rows
// requested too long ago, and nothing bounds a row stamped ahead of the host
// doing the reading. Both stamps on a sample come from whichever plane host
// handled that handoff, through the board transaction that committed it, so
// the reader and the writer are not the same clock and the plane already says
// so: board.MaxClockOffset is the offset the fence tolerates between two hosts
// of this control plane, and the estimator reuses it when it refuses a
// measured handoff that reaches neutral in the future.
//
// That refusal leaves two doors open, and requested_at is the ORDER BY key of
// this read, so a row stamped ahead of the plane sits at the head of every
// page of its cohort and stays inside the cutoff window until the clock
// catches up to it, which for a badly-set host is months.
//
//   - A CENSORED row is exempt from the estimator's check entirely
//     (validateYieldSample returns early on a nonempty exclusion reason,
//     deliberately, because a request that never reached neutral has no
//     neutral time to judge). Nothing else looks at its request stamp. It is
//     counted into the Censored total that Provenance prints beside the ETA,
//     where a person reads it as evidence that this board keeps failing to
//     hand off.
//   - A MEASURED row's request stamp is never compared to now at all. The
//     pair check catches the ordinary case, because a neutral time after a
//     future request is itself in the future, but it catches it one layer
//     later and reports it as a broken estimate for the whole cohort rather
//     than as the one unusable row it is.
//
// So the rule is applied here, at the read, to every row and to both stamps,
// and it is loud for the reason yieldSampleFrom is loud: a row this plane's
// own clocks cannot account for means the history and the host disagree, and
// quietly dropping it hides that from the line the requester is shown.

// yieldSampleFitsTheClock refuses a stored row stamped further ahead of the
// reading host than the plane's own clock tolerance allows.
func yieldSampleFitsTheClock(row yieldSampleRow, now time.Time) error {
	if now.IsZero() {
		return fmt.Errorf("%w: yield sample needs the current time to be judged against", ErrInvalid)
	}
	// The same horizon the estimator uses, so the read and the estimate
	// cannot disagree about which stamps are believable.
	horizon := now.Add(board.MaxClockOffset)
	if row.RequestedAt.After(horizon) {
		return fmt.Errorf("%w: stored yield sample %s is requested in the future", ErrConflict, row.LeaseID)
	}
	// A censored row carries no neutral time and is judged on its request
	// stamp alone; a zero stamp here is the absence of a measurement, not a
	// stamp from 1970.
	if !row.NeutralAt.IsZero() && row.NeutralAt.After(horizon) {
		return fmt.Errorf("%w: stored yield sample %s reaches neutral in the future", ErrConflict, row.LeaseID)
	}
	return nil
}
