// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package syncclient

import (
	"errors"
	"fmt"
	"unicode/utf8"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/spool"
)

// maxFilableSteps is local_runs' step bound, and maxFilableStepKeyBytes is
// local_steps.key's, both restated rather than imported for the reason every
// other bound in this package is restated: internal/store does not depend on
// this package, and a door that can only refuse what a live plane would
// refuse has to know the numbers the plane holds. store.validateLocalRun
// refuses more than 128 steps and a key outside 1..128 bytes.
const (
	maxFilableSteps        = 128
	maxFilableStepKeyBytes = 128
)

// ErrUnfilableSteps is the refusal of a record whose steps the plane has no
// rows to file.
var ErrUnfilableSteps = errors.New("local record carries steps the plane will not file")

// checkUploadedStepsAreOnesThePlaneWillFile holds a record's steps to the
// rows the plane can actually write for them.
//
// The sweep already asks two questions of each step, both about what the step
// measured: the exit code it reports (#1953) and the log evidence it states
// (#1955). It has never asked about the step's own IDENTITY, the one field
// that becomes the row's primary text: executor.StepResult.Name, which
// server.offlineInput copies into store.LocalStepInput.Key verbatim, and it
// has never bounded how MANY steps a record may carry.
//
// Nothing else on this host asks either. spool.checkStepWindowsFitTheRecord
// holds every step's stamps to the record's envelope and stops there, by its
// own account; spool.Pending reads the file back through json.Unmarshal,
// which fills whatever the file holds, so a record written by an older
// client, or a file edited between the freeze and the sweep, is uploaded with
// whatever names and however many steps it states.
//
// THE RULE, the store's own (validateLocalRun): at most 128 steps, each key
// 1..128 bytes, no two keys the same, and each key text a text column can
// hold (valid UTF-8, no C0 control, no DEL, no C1). The names come from the
// reviewed definition on an honest run, so a record this build writes cannot
// be refused here; what this door catches is a record that no longer says
// what a definition said.
//
// DELIBERATELY NOT the comparison server.offlineInput makes, step.Name
// against definition.Steps[i].Name with len(Steps) <= len(definition.Steps).
// That needs the reviewed catalog, which a spooled record carries only the
// DIGEST of, the same cut the catalog-digest door (#1956), the classification
// door (#1957) and the argument door (#1961) each make: a client guessing at
// a catalog it does not hold would refuse evidence the plane would have
// taken. The ordinal is not restated either, for a different reason: nothing
// uploads one. offlineInput assigns it from its own loop index, so it is
// correct by construction and there is no field here to be wrong.
//
// Refused here, the operator reads the step and what is wrong with its name.
// Left to the far end it is the shape this package has now chosen ten times:
// SyncPending returns on the first response that is not 200, so one
// unfilable record at the front of the outbox holds every record behind it
// on this pass and on every pass after, and the operator reads back "upload
// local <id> returned HTTP 400" with no field named.
func checkUploadedStepsAreOnesThePlaneWillFile(entry spool.Entry) error {
	if entry.Result == nil {
		return nil
	}
	if len(entry.Result.Steps) > maxFilableSteps {
		return fmt.Errorf("%w: %d steps, more than the %d the plane files",
			ErrUnfilableSteps, len(entry.Result.Steps), maxFilableSteps)
	}
	seen := make(map[string]bool, len(entry.Result.Steps))
	for i, step := range entry.Result.Steps {
		switch {
		case step.Name == "":
			return fmt.Errorf("%w: step %d states no name", ErrUnfilableSteps, i)
		case len(step.Name) > maxFilableStepKeyBytes:
			return fmt.Errorf("%w: step %d names %d bytes, more than the %d the plane files",
				ErrUnfilableSteps, i, len(step.Name), maxFilableStepKeyBytes)
		case !stepKeyThePlaneCanFile(step.Name):
			return fmt.Errorf("%w: step %d states a name no text column can hold", ErrUnfilableSteps, i)
		case seen[step.Name]:
			return fmt.Errorf("%w: step %d repeats the name %q", ErrUnfilableSteps, i, step.Name)
		}
		seen[step.Name] = true
	}
	return nil
}

// stepKeyThePlaneCanFile restates store.namesATextColumnCanHold: valid UTF-8
// with no C0 control, no DEL and no C1 control. A tab or a newline inside a
// column key is refused by the plane, so it is refused here.
func stepKeyThePlaneCanFile(key string) bool {
	if !utf8.ValidString(key) {
		return false
	}
	for _, char := range key {
		if char < 0x20 || char == 0x7f || (char >= 0x80 && char <= 0x9f) {
			return false
		}
	}
	return true
}
