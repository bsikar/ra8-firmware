// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package spool

import (
	"errors"
	"fmt"
	"unicode/utf8"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/executor"
)

const (
	// maxMeasurableSteps is the store's own ceiling on how many steps one
	// local run may file (store.validateLocalRun, len(in.Steps) > 128).
	maxMeasurableSteps = 128
	// maxMeasuredStepNameBytes is the store's own ceiling on a step key, in
	// BYTES rather than runes, because the column is sized in bytes.
	maxMeasuredStepNameBytes = 128
)

// errUnnameableStep names the one thing this rule refuses: a result handed to
// Finish whose steps cannot be told apart in the row the plane files them in.
var errUnnameableStep = errors.New("execution result states a step the plane cannot name")

// checkEachStepCanBeNamed holds a result's steps to names the plane can file,
// at the freeze.
//
// The freeze already asks what each step MEASURED (checkTheEvidenceWasMeasured,
// checkTheExitsWereReported) and how it STOPPED (checkEachStepEndedOneWay), and
// has never asked who the step IS. server.offlineInput copies StepResult.Name
// into store.LocalStepInput.Key verbatim (offline.go:161) and that key is the
// row's whole identity: it is what an operator reads back, what a later run is
// compared against, and the only thing distinguishing one row of local_steps
// from the next.
//
// THE RULE, and it is the store's rather than a new one
// (store.validateLocalRun, local_sync.go:227-228): at most 128 steps, each key
// 1 to 128 bytes, no two keys the same, and each key text a text column can
// hold, which is valid UTF-8 with no C0 control, no DEL and no C1. The bound is
// bytes, so 128 two-byte runes is over it.
//
// This is the FREEZE, not the sweep, and the two doors are not the same door.
// checkUploadedStepsAreOnesThePlaneWillFile judges a record read back OFF DISK,
// where an older build's file or an edited one can say anything at all; this one
// judges what the executor in this process just handed over, before the bytes
// are written, so the outbox never holds the claim in the first place. Refused
// at the sweep instead, the record is read, posted, refused with an opaque 400
// and left in the outbox, and every unsynced record behind it waits on every
// pass.
//
// DELIBERATELY NOT the comparison against the reviewed definition that
// server.offlineInput makes (step.Name != definition.Steps[i].Name, more steps
// than the definition has). That needs the catalog, which a spooled record
// carries only the digest of, so a freeze guessing at it would refuse evidence
// the plane would have taken. DELIBERATELY NOT the ordinal either: offlineInput
// assigns it from its own loop index, so nothing here states one to be wrong
// about.
func checkEachStepCanBeNamed(result executor.Result) error {
	if len(result.Steps) > maxMeasurableSteps {
		return fmt.Errorf("%w: the result states %d steps, more than the %d the plane files",
			errUnnameableStep, len(result.Steps), maxMeasurableSteps)
	}
	seen := make(map[string]int, len(result.Steps))
	for i, step := range result.Steps {
		if len(step.Name) == 0 {
			return fmt.Errorf("%w: %s states no name at all", errUnnameableStep, namedStepOf(result, i))
		}
		if len(step.Name) > maxMeasuredStepNameBytes {
			return fmt.Errorf("%w: %s states a name of %d bytes, over the %d the plane files",
				errUnnameableStep, namedStepOf(result, i), len(step.Name), maxMeasuredStepNameBytes)
		}
		if !stepNameThePlaneCanFile(step.Name) {
			return fmt.Errorf("%w: %s states a name no text column can hold",
				errUnnameableStep, namedStepOf(result, i))
		}
		if first, ok := seen[step.Name]; ok {
			return fmt.Errorf("%w: %s states the name step %d already stated",
				errUnnameableStep, namedStepOf(result, i), first)
		}
		seen[step.Name] = i
	}
	return nil
}

// stepNameThePlaneCanFile restates store.namesATextColumnCanHold: valid UTF-8,
// no C0 control, no DEL, no C1. It is restated rather than imported so this
// package does not depend on the store to freeze a local record.
func stepNameThePlaneCanFile(name string) bool {
	if !utf8.ValidString(name) {
		return false
	}
	for _, char := range name {
		if char < 0x20 || char == 0x7f || (char >= 0x80 && char <= 0x9f) {
			return false
		}
	}
	return true
}
