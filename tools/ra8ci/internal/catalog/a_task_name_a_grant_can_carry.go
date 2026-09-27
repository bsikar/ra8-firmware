// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import "fmt"

// A reviewed task is reached exactly one way: a grant names it. protocol
// refuses an assignment whose task name is empty or longer than 64 bytes
// (taskNameNamesAReviewedTask, a_task_name_the_grant_can_name.go), and that
// refusal happens at the message boundary, before any catalog is loaded.
//
// The catalog bounded the same name at 128 bytes, because that is what durable
// history files (a_task_history_can_file.go, restating the store). The two
// bounds are different numbers for different reasons, and between them sits a
// definition nothing refuses and nothing can run: a reviewed task named 65 to
// 128 bytes is admitted, digested into the catalog, listed by the dispatcher,
// and then every grant naming it is refused at the far end as a malformed
// message. The task is unreachable and the diagnosis an operator reads is about
// the message, not about the definition that can never be dispatched.
//
// So the narrower of the two bounds belongs at admission, where a reviewer
// finds out at review time rather than at dispatch time. Admission, not the
// runtime re-check in ValidateTask: an agent holding such a task got it through
// a grant that already passed the protocol bound, so the re-check can never see
// this shape and paying for it per attempt buys nothing.
//
// The bound is RESTATED, not imported. protocol states it as the boundary that
// must judge a message with no catalog loaded; this package must not depend on
// the wire format to admit a definition. The longest task name in the tree
// today is 29 bytes ("inclusive-terminology-commits"), so a new reviewed task
// is nowhere near being refused for its length alone.
//
// Deliberately NOT the alphabet or the empty name: validName already holds a
// task name to lowercase letters, digits and hyphens and refuses an empty one,
// which is exactly what the protocol accepts, so only the LENGTH was open.
// Deliberately NOT step names, which no grant carries: a grant names the task,
// and history is the only thing that files a step name.
const maxGrantableTaskNameBytes = 64

// checkTheTaskNameIsOneAGrantCanCarry refuses a reviewed definition no grant
// could ever name.
func checkTheTaskNameIsOneAGrantCanCarry(task Task) error {
	if len(task.Name) > maxGrantableTaskNameBytes {
		return fmt.Errorf("%w: task name is %d bytes, more than the %d a grant can carry, so no assignment could name it",
			ErrInvalidCatalog, len(task.Name), maxGrantableTaskNameBytes)
	}
	return nil
}
