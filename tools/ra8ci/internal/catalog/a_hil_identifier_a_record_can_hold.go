package catalog

import "fmt"

// maxRecordableHILIdentifierBytes is the width of the text columns a HIL
// workload's identifiers are filed into, restated here rather than imported.
//
// hil_observations bounds program_family at 1..128 bytes in its own CHECK
// (migrations/0014_hil_observations.sql) and store.validHILWorkload restates
// that in Go; board_neutral_challenge bounds board_id the same way
// (migrations/0003_board_neutral_challenge.sql), and the board CLI's own
// argument rule (validBoardIDArgument) stops at 128 too. This package must not
// depend on the store to judge a definition, so the number is stated, not
// imported, and it is the same number on purpose.
const maxRecordableHILIdentifierBytes = 128

// checkTheHILIdentifiersAreOnesARecordCanHold refuses a reviewed HIL task
// whose board or program identity is wider than the row its evidence lands in.
//
// validName already holds both fields to [a-z0-9-], which is every axis these
// identifiers have EXCEPT length: it accepts a name of any size, so a
// definition naming a 200-byte program family was admitted by review, digested
// into the catalog, dispatched to a guest, and the board was granted, flashed
// and observed before the INSERT refused it. That refusal arrives as an
// unavailable store, long after the work is done and with the observation
// unfileable, when the definition could have been refused for free.
//
// It sits beside the board-model and manifest-path rules: those two hold the
// free-text halves of the hil_observations conflict key to text a record can
// carry, and this one holds the two identifier halves to a width a record can
// hold. Mode is the remaining key field and needs no rule, since
// ValidateHILTaskMetadata already closes it to six literal spellings.
//
// The observation step is DELIBERATELY not judged here. It must equal one of
// the task's own step names, and checkTheTaskIsOneHistoryCanFile already bounds
// every step name at maxFilableNameBytes, so a step key wider than its column
// cannot reach a record through this field.
func checkTheHILIdentifiersAreOnesARecordCanHold(task Task) error {
	if task.HIL == nil {
		return nil
	}
	if len(task.HIL.ProgramFamily) > maxRecordableHILIdentifierBytes {
		return fmt.Errorf("%w: program family for %q is %d bytes, wider than the %d a record can hold",
			ErrInvalidCatalog, task.Name, len(task.HIL.ProgramFamily), maxRecordableHILIdentifierBytes)
	}
	if len(task.HIL.BoardID) > maxRecordableHILIdentifierBytes {
		return fmt.Errorf("%w: board id for %q is %d bytes, wider than the %d a record can hold",
			ErrInvalidCatalog, task.Name, len(task.HIL.BoardID), maxRecordableHILIdentifierBytes)
	}
	return nil
}
