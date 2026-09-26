package server

import (
	"fmt"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/executor"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/spool"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// Holding a spooled step to the envelope its own record states.
//
// checkLocalEnvelopeIsStated bounds the RECORD: two stamps that can be
// subtracted, a span no longer than any reviewed deadline. The steps inside it
// were never bounded by anything. offlineInput matches each step's name to the
// reviewed definition and then copies StartedAt, EndedAt and Duration into
// store.LocalStepInput verbatim, and all three arrive as client JSON.
//
// The damage is the same one the record-level rule exists for, one level down.
// A step whose stated duration is math.MaxInt64 nanoseconds lands in durable
// history as the length of that step; a step stamped outside the run that
// contains it describes work the record never claimed to have been doing. The
// run's own verdict does not catch either: downgrading Result to
// incomplete_evidence says the ATTEMPT proved nothing, and the uninterpretable
// step row is written either way. So this refuses rather than downgrades.
//
// The record's envelope is the bound rather than the executor result's,
// because the record's is the only one guaranteed to be stated: offlineInput
// tolerates a zero Result.StartedAt (the `!entry.Result.StartedAt.IsZero()`
// clause), while checkLocalEnvelopeIsStated has already refused a record
// missing either of its own stamps by the time any step is read.
func checkLocalStepIsStated(step executor.StepResult, entry spool.Entry) error {
	if entry.FinishedAt == nil || entry.FinishedAt.IsZero() || entry.StartedAt.IsZero() {
		return fmt.Errorf("%w: step %q cannot be judged against a record stating no envelope",
			store.ErrInvalid, step.Name)
	}
	if step.StartedAt.IsZero() || step.EndedAt.IsZero() {
		return fmt.Errorf("%w: step %q states no start or no end", store.ErrInvalid, step.Name)
	}
	if step.EndedAt.Before(step.StartedAt) {
		return fmt.Errorf("%w: step %q ended before it started", store.ErrInvalid, step.Name)
	}
	if step.StartedAt.Before(entry.StartedAt) || step.EndedAt.After(*entry.FinishedAt) {
		return fmt.Errorf("%w: step %q ran outside the record's envelope", store.ErrInvalid, step.Name)
	}
	// The stated duration is bounded by the envelope, not held equal to the
	// step's own stamps. The executor measures Duration on the monotonic
	// clock (end.Sub(started)) while the stamps are wall-clock conversions
	// of those same instants, so a wall-clock adjustment mid-step makes the
	// two disagree honestly, by a real amount, in either direction.
	if envelope := entry.FinishedAt.Sub(entry.StartedAt); step.Duration < 0 || step.Duration > envelope {
		return fmt.Errorf("%w: step %q states a duration of %s, outside the record's %s envelope",
			store.ErrInvalid, step.Name, step.Duration, envelope)
	}
	return nil
}
