// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package demand

import "fmt"

// checkTimestampsAgree holds one event's payload timestamps to the order the
// job they describe can actually have happened in: queued, then started,
// then completed.
//
// All three come from the delivery body, never from this plane: created_at,
// started_at and completed_at are fields of workflowJobDelivery, and
// ObservedAt is the only stamp on an Event that a sender cannot choose. So
// the only thing standing between a sender's arithmetic and durable history
// is this rule. Validate already refused the completed-before-queued case;
// the other two arms of the same triangle went unstated, and a stored event
// whose stamps disagree is not a wrong number in one column, it is a row
// that reads like evidence about a job that ran before it was asked for.
// Downstream, a queue wait is StartedAt minus QueuedAt and a run duration is
// CompletedAt minus StartedAt, and both go negative off such a row.
//
// Equal stamps are accepted throughout. A job queued and started inside the
// same second is ordinary, and GitHub's own timestamps are second-resolution,
// so only a strictly earlier stamp is a disagreement. A zero stamp is absent
// rather than early: whether it is allowed to be absent is Validate's
// question, asked before this one.
func checkTimestampsAgree(e Event) error {
	if !e.StartedAt.IsZero() && e.StartedAt.Before(e.QueuedAt) {
		return fmt.Errorf("%w: started before queued", ErrInvalid)
	}
	// The wording of this one is pinned by an existing test and by every
	// log line written since, so it stays exactly as it was.
	if !e.CompletedAt.IsZero() && e.CompletedAt.Before(e.QueuedAt) {
		return fmt.Errorf("%w: completed before queued", ErrInvalid)
	}
	if !e.CompletedAt.IsZero() && !e.StartedAt.IsZero() && e.CompletedAt.Before(e.StartedAt) {
		return fmt.Errorf("%w: completed before started", ErrInvalid)
	}
	return nil
}
