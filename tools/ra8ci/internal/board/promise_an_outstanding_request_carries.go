package board

import "time"

// The ETA a requester was shown for a handoff that was already in flight.
//
// requestYield records the shown target and its cohort in one place: the arm
// that moves an Active board to YieldRequested. That arm is deliberately the
// only writer, because a repeat request against a board already asked must not
// overwrite the number the first requester was given; that is how a deadline
// slides without anyone deciding to move it.
//
// What the arm also covers, without meaning to, is the case where there is no
// number to protect. A yield this state machine raised itself records none:
// enqueue stamps YieldRequestedAt when a higher-priority waiter arrives and
// leaves HandoffTarget and HandoffCohort zero, which HandoffTarget's own doc
// comment calls the honest answer for a yield nobody was shown an ETA for. A
// requester arriving after that is admitted by admitYield, is quoted an ETA by
// PlanYield against the stamp already on the lease, issues RequestYield
// carrying that target and cohort, and is told the command succeeded. The
// phase is YieldRequested, so the whole recording arm is skipped and both
// values are dropped on the floor.
//
// The loss is the exact harm the fields exist to prevent. PlanYield anchors
// RequestedAt to the existing stamp, so the anchor holds, but promised() only
// pins the target when the lease carries one; with nothing recorded, every
// poll re-estimates over whatever history has arrived since, and the requester
// refreshing the page watches the deadline move. The cohort goes the same way:
// YieldSampleFor files the completed measurement against whatever cohort the
// caller derives at completion time, which is the bucket the board is in by
// then rather than the one the quoted ETA came from.
//
// So the rule is adoption, not overwrite, and the two are distinguished by
// what is already on record rather than by the phase. A lease carrying neither
// a target nor a cohort has no promise to protect, and the first ETA a
// requester is actually shown becomes the promise. A lease carrying either one
// keeps it, and a later caller's number is dropped exactly as it is today.
//
// No second YieldAsked is emitted. The audit trail's question is when this
// board was asked to yield, that moment is already on record with the stamp
// the handoff clock runs from, and a second request event would put two
// answers in the trail for one handoff. Nothing was asked here; a promise the
// lease was already able to hold was written down.
func adoptShownPromise(lease *Lease, target time.Duration, cohort YieldCohort) bool {
	if lease == nil || lease.YieldRequestedAt.IsZero() {
		return false
	}
	if lease.HandoffTarget != 0 || lease.HandoffCohort != (YieldCohort{}) {
		return false
	}
	if target == 0 && cohort == (YieldCohort{}) {
		return false
	}
	lease.HandoffTarget = target
	lease.HandoffCohort = cohort
	return true
}
