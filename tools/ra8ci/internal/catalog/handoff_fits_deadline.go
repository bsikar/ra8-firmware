// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import "fmt"

// checkSafeStepFitsTheDeadline refuses a reviewed HIL task whose longest
// indivisible step outlasts the deadline the attempt runs under.
//
// validateHandoffBounds already states this rule against the optional attempt
// cap: an indivisible step cannot outlast the cap on the whole attempt it runs
// inside, because such a task could be asked to yield in the middle of a step
// the safety maximum would already have killed. It states it against
// SafetyMaximumSeconds, which a task may leave undeclared, and it is handed
// only the HIL block, so the cap that ALWAYS exists is outside what it can
// see: DeadlineSeconds, on the task itself.
//
// That deadline is not advisory. The executor runs the whole task under a
// context cancelled at DeadlineSeconds (executor.Run), so a step declared
// longer than it is a step that cannot finish. The cost lands on the handoff,
// which is what the bound exists for: the safe step is the floor under a
// quoted request-to-neutral ETA (board.DeclaredHandoffBounds.SafetyBound),
// so a waiter is told to expect the board back no sooner than a step the
// plane will have cancelled first, and automatic dispatch is held off for
// that whole quoted window.
//
// Only the safe step is judged here, not the sum. The restore probe is the
// work that follows the step, and this tree already budgets a restore OUTSIDE
// the attempt deadline (store.attempts adds FlashRestoreSeconds to the board
// hold rather than to the attempt), so a safety bound reaching past the
// deadline is ordinary rather than a contradiction.
//
// This is an admission rule, applied where a manifest is read, not a re-check
// of a task already persisted against a reviewed digest: it reads a field
// outside the HIL block, and refusing a held definition on a cross-field rule
// the runtime never applied before is the retroactive refusal ValidateTask's
// own comment draws the line against. A definition admitted under an older
// rule keeps running.
//
// Precedent for holding a HIL number under the task deadline:
// boardclient.validHILTimingAssignment refuses a timing decision whose
// validity window exceeds DeadlineSeconds, on the same reasoning at the other
// end of the same contract.
func checkSafeStepFitsTheDeadline(task Task) error {
	if task.HIL == nil || !task.HIL.HandoffBoundsDeclared() {
		return nil
	}
	if task.HIL.HandoffSafeStepSeconds > task.DeadlineSeconds {
		return fmt.Errorf("%w: HIL task %q declares a %ds indivisible step inside a %ds deadline; a step the attempt deadline cancels cannot be the floor under a handoff ETA",
			ErrInvalidCatalog, task.Name, task.HIL.HandoffSafeStepSeconds, task.DeadlineSeconds)
	}
	return nil
}
