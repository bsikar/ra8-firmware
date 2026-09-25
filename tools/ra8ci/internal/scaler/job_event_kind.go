// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package scaler

import (
	"github.com/actions/scaleset"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/github"
)

// A scale-set job event carries the same identity fields whichever transition
// it reports, so its kind is the only field separating "this job has picked up
// its runner" from "this job has finished". Every step that turns an event into
// durable evidence names the kind it needs here rather than reading the field
// itself, so the two rules cannot drift apart.

// provesRunnerStarted reports whether the event is GitHub's statement that the
// job began on its runner, the only kind that may back registration evidence.
func provesRunnerStarted(job github.Job) bool {
	return job.Kind == scaleset.MessageTypeJobStarted
}

// provesJobCompleted reports whether the event is GitHub's terminal statement
// for the job, the only kind that may back drain and destroy evidence.
func provesJobCompleted(job github.Job) bool {
	return job.Kind == scaleset.MessageTypeJobCompleted
}
