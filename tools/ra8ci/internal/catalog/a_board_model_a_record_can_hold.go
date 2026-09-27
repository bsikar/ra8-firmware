// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import (
	"fmt"
	"unicode/utf8"
)

// checkTheBoardModelIsOneARecordCanHold holds the one free-text field a
// reviewed HIL contract states to text a record can carry.
//
// A HIL task names its board twice. BoardID is an identifier and validName
// holds it to lowercase letters, digits and dashes. BoardModel is the human
// half, the model an operator reads back ("EK-RA8D2"), and
// ValidateHILTaskMetadata asked only that it be non-empty, carry no
// surrounding whitespace and stay inside 128 bytes. Every byte in between was
// admitted, invalid UTF-8 and the control range included.
//
// That string does not stay in the definition. It travels in the granted
// workload (hilspec.Workload), the board client compares it byte for byte
// against the task it holds, and the store files it as a column keyed on:
// hil_observations conflicts on (attempt_id, manifest_path, board_model,
// fixture_revision, profile_sha256, program_family, mode), and
// board_yield_samples and board_yield_history carry the same value as the
// cohort a handoff estimate is drawn from. A Postgres text column holds no NUL
// byte and no invalid UTF-8 whatever its length, so those two spellings were
// not refused where they could be refused cheaply; they reached the INSERT
// after the board had already been held, flashed and observed, which reports
// work that genuinely happened as an unavailable store rather than as the
// invalid definition it came from.
//
// The rest of the control range is the reading half, and the reasoning is the
// store's own for the names it files runs under (namesATextColumnCanHold in
// text_a_text_column_can_hold.go): this is the string an operator matches a
// board's history by, a model carrying a newline reads as two cohorts in
// anything line-oriented, and one carrying an escape sequence rewrites the
// terminal that prints it. C1 is refused beside C0 for the reason the agent
// boundary refuses it: a name that renders as nothing is a name no one can
// match by reading it, and these values are compared byte for byte against
// rows already stored.
//
// This is an admission rule and not part of ValidateTask's runtime re-check,
// for the reason ValidateTask states: a runtime holding this task was granted
// it against a reviewed digest, so refusing it there would retroactively
// refuse work review already admitted.
func checkTheBoardModelIsOneARecordCanHold(task Task) error {
	if task.HIL == nil {
		return nil
	}
	if !boardModelARecordCanHold(task.HIL.BoardModel) {
		return fmt.Errorf("%w: task %q declares a board model no record could hold: %q",
			ErrInvalidCatalog, task.Name, task.HIL.BoardModel)
	}
	return nil
}

// boardModelARecordCanHold reports whether a reviewed board model is text this
// store can write and a reader can read back.
//
// It deliberately states no alphabet. A model is a vendor's spelling of its
// own hardware, so this package has no standing to narrow it past what a
// record requires; the length and whitespace bounds it already had stay where
// they are, in ValidateHILTaskMetadata.
func boardModelARecordCanHold(model string) bool {
	if !utf8.ValidString(model) {
		return false
	}
	for _, char := range model {
		if char < 0x20 || char == 0x7f || (char >= 0x80 && char <= 0x9f) {
			return false
		}
	}
	return true
}
