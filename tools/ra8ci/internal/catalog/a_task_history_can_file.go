// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import "fmt"

// A reviewed task states two things durable history has a column for: the name
// the run row is filed under, and the name of every step row beneath it. Both
// columns are bounded, and the catalog bounded neither.
//
// The rule restated here is the store's (validateLocalRun in
// internal/store/local_sync.go): a task name of 1..128 bytes, at most 128
// steps, and a step key of 1..128 bytes. The spool states the same bounds at
// the freeze (checkEachStepCanBeNamed) and the sweep states them on the way
// out (checkUploadedStepsAreOnesThePlaneWillFile). All three judge a RECORD,
// after the work is done. Nothing judged the DEFINITION, so a task declaring
// 200 steps, or one step named at 300 bytes, was admitted by review, dispatched
// to a guest, run to completion, and only then found unfilable, once per
// attempt, forever. The cost is not a bad row; it is work whose evidence can
// never be filed.
//
// The lower bound and the shape of a name are not restated: validName already
// refuses an empty one and holds every name to lowercase letters, digits and
// the hyphen, which is narrower than namesATextColumnCanHold on every axis.
// Only the LENGTH was unbounded, so only the length is stated here. Bytes, not
// runes, because the column counts bytes; validName makes the two the same
// today, and saying bytes keeps this rule right if it ever widens.
//
// Deliberately NOT the step ARGV: a step's arguments are what the guest runs,
// not something offlineInput files, so no history column bounds them and this
// door has nothing to say about them.
const (
	maxFilableSteps     = 128
	maxFilableNameBytes = 128
)

// checkTheTaskIsOneHistoryCanFile refuses a reviewed definition no completed
// run of which could be filed.
func checkTheTaskIsOneHistoryCanFile(task Task) error {
	if len(task.Name) > maxFilableNameBytes {
		return fmt.Errorf("%w: task name is %d bytes, more than the %d durable history files",
			ErrInvalidCatalog, len(task.Name), maxFilableNameBytes)
	}
	if len(task.Steps) > maxFilableSteps {
		return fmt.Errorf("%w: task %q declares %d step(s), more than the %d durable history files",
			ErrInvalidCatalog, task.Name, len(task.Steps), maxFilableSteps)
	}
	for _, step := range task.Steps {
		if len(step.Name) > maxFilableNameBytes {
			return fmt.Errorf("%w: task %q step name is %d bytes, more than the %d durable history files",
				ErrInvalidCatalog, task.Name, len(step.Name), maxFilableNameBytes)
		}
	}
	return nil
}
