// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import (
	"fmt"
	"unicode/utf8"
)

const (
	// maxDeclaredArgumentsPerStep bounds the literal argv a reviewed step
	// states. The widest step in the tree today declares three
	// (scripts/ci.sh --gate <name>), and the widest a caller can add is the
	// schema's own MaxPositionalArguments + MaxFlagArguments, which is 24,
	// so 32 is headroom for a new reviewed step rather than a ceiling any
	// existing one is near.
	maxDeclaredArgumentsPerStep = 32
	// maxDeclaredArgumentBytes bounds one declared argv element. It is
	// MaxArgumentValueBytes restated: a declared literal and a supplied
	// value become the same kind of thing, one element of one argv, and
	// there is no reason the reviewed half may be wider than the caller's.
	// The longest in the tree today is 29 bytes.
	maxDeclaredArgumentBytes = MaxArgumentValueBytes
)

// errUnreadableDeclaredArgument names the one thing this rule refuses: a
// reviewed step whose own literal arguments cannot be read back as evidence of
// what was run.
var errUnreadableDeclaredArgument = fmt.Errorf("%w: reviewed step declares an argument review cannot read", ErrInvalidCatalog)

// checkEachDeclaredArgumentIsOneReviewCanRead holds a step's literal argv to
// text that can be read back, at admission.
//
// THE GAP is one line of ValidateTask. That loop refuses a step whose Program
// carries a NUL, a carriage return or a newline, and then refuses a step whose
// Args carry a NUL and nothing else. Program and Args are the same argv: the
// executor passes the one as the command and the others as its elements, the
// whole line is what an operator reads back out of a log or an audit row, and
// there is no reason the first element answers for its text while the rest do
// not. A reviewed step declaring "--gate\nrm -rf /" was admitted, dispatched,
// run, and filed, and the evidence of it reads as two lines.
//
// THE RULE is the store's own text rule restated
// (store.namesATextColumnCanHold, text_a_text_column_can_hold.go, which
// spool.stepNameThePlaneCanFile already restates for a step's name): valid
// UTF-8, no C0 control, no DEL, no C1. Plus a length, which is
// MaxArgumentValueBytes, and a count. It is restated rather than imported so
// the catalog keeps no dependency on the store to admit a definition.
//
// DELIBERATELY NOT ValidArgumentValue, which is the rule for a value a CALLER
// supplies. That one refuses a leading dash and every shell metacharacter,
// and a declared literal is the one place both are ordinary: --gate is a flag
// the reviewed step means to pass, and the argv shape means nothing re-splits
// it. Applying the caller's rule here would refuse most of the tree. The two
// halves of an argv are held to the same TEXT, not to the same alphabet.
//
// It is an ADMISSION rule (ValidateReviewedTask) and not part of the runtime
// re-check, for the reason ValidateTask states about the dispatch seam: an
// agent re-checking a task it already holds was granted it against a reviewed
// digest, possibly under an older rule, and a re-check folding this in would
// refuse work review already admitted.
func checkEachDeclaredArgumentIsOneReviewCanRead(task Task) error {
	for _, step := range task.Steps {
		if len(step.Args) > maxDeclaredArgumentsPerStep {
			return fmt.Errorf("%w: step %q of task %q declares %d arguments, over the %d a reviewed step may",
				errUnreadableDeclaredArgument, step.Name, task.Name, len(step.Args), maxDeclaredArgumentsPerStep)
		}
		for index, argument := range step.Args {
			if len(argument) == 0 {
				return fmt.Errorf("%w: argument %d of step %q of task %q is empty",
					errUnreadableDeclaredArgument, index, step.Name, task.Name)
			}
			if len(argument) > maxDeclaredArgumentBytes {
				return fmt.Errorf("%w: argument %d of step %q of task %q is %d bytes, over the %d a reviewed step may declare",
					errUnreadableDeclaredArgument, index, step.Name, task.Name, len(argument), maxDeclaredArgumentBytes)
			}
			if !argumentTextReviewCanRead(argument) {
				return fmt.Errorf("%w: argument %d of step %q of task %q states text no record can hold",
					errUnreadableDeclaredArgument, index, step.Name, task.Name)
			}
		}
	}
	return nil
}

// argumentTextReviewCanRead restates store.namesATextColumnCanHold: valid
// UTF-8, no C0 control, no DEL, no C1.
func argumentTextReviewCanRead(value string) bool {
	if !utf8.ValidString(value) {
		return false
	}
	for _, char := range value {
		if char < 0x20 || char == 0x7f || (char >= 0x80 && char <= 0x9f) {
			return false
		}
	}
	return true
}
