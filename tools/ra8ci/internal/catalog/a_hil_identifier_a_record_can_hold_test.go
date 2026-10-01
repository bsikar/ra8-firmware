package catalog

import (
	"errors"
	"strings"
	"testing"
)

// identifiedTask starts from the fixture the observation rules use and asserts
// it is admitted BEFORE the rule under test is exercised, so a later failure is
// this rule's answer and not a fixture that drifted.
func identifiedTask(t *testing.T) Task {
	t.Helper()
	task := observedHILTask(t)
	if err := ValidateReviewedTask(task); err != nil {
		t.Fatalf("the fixture this rule is measured against is not admitted: %v", err)
	}
	return task
}

func nameOfBytes(count int) string {
	return strings.Repeat("a", count)
}

func TestAProgramFamilyWiderThanARecordIsRefused(t *testing.T) {
	task := identifiedTask(t)
	task.HIL.ProgramFamily = nameOfBytes(maxRecordableHILIdentifierBytes + 1)
	err := ValidateReviewedTask(task)
	if !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("a program family wider than its column was accepted: %v", err)
	}
	if !strings.Contains(err.Error(), "program family") {
		t.Fatalf("refusal does not name the field: %v", err)
	}
	if !strings.Contains(err.Error(), task.Name) {
		t.Fatalf("refusal does not name the task: %v", err)
	}
}

func TestABoardIDWiderThanARecordIsRefused(t *testing.T) {
	task := identifiedTask(t)
	task.HIL.BoardID = nameOfBytes(maxRecordableHILIdentifierBytes + 1)
	err := ValidateReviewedTask(task)
	if !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("a board id wider than its column was accepted: %v", err)
	}
	if !strings.Contains(err.Error(), "board id") {
		t.Fatalf("refusal does not name the field: %v", err)
	}
}

// The boundary is inclusive: the column holds 128 bytes, so a definition
// stating exactly 128 is fileable and must stay admitted.
func TestIdentifiersExactlyAsWideAsTheRecordAreAdmitted(t *testing.T) {
	task := identifiedTask(t)
	task.HIL.ProgramFamily = nameOfBytes(maxRecordableHILIdentifierBytes)
	task.HIL.BoardID = nameOfBytes(maxRecordableHILIdentifierBytes)
	if err := ValidateReviewedTask(task); err != nil {
		t.Fatalf("identifiers exactly as wide as the record were rejected: %v", err)
	}
}

// The refusal reports the width it read and the width it allows, because the
// operator fixing the manifest needs to know how far over it is.
func TestTheRefusalNamesBothWidths(t *testing.T) {
	task := identifiedTask(t)
	task.HIL.ProgramFamily = nameOfBytes(200)
	err := ValidateReviewedTask(task)
	if err == nil {
		t.Fatal("a 200 byte program family was accepted")
	}
	if !strings.Contains(err.Error(), "200") || !strings.Contains(err.Error(), "128") {
		t.Fatalf("refusal does not name both widths: %v", err)
	}
}

// This rule judges width only. The alphabet is validName's answer and must stay
// validName's answer, reported as an invalid HIL contract rather than as a
// width this rule never measured.
func TestTheWidthDoorDoesNotChooseAnAlphabet(t *testing.T) {
	task := identifiedTask(t)
	task.HIL.ProgramFamily = "Camera_Family"
	err := ValidateReviewedTask(task)
	if !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("an invalid program family alphabet was accepted: %v", err)
	}
	if strings.Contains(err.Error(), "wider than") {
		t.Fatalf("the width door answered for the alphabet: %v", err)
	}
}

// An empty identifier is validName's refusal too, for the same reason.
func TestAnEmptyProgramFamilyIsStillTheNameRuleAnswer(t *testing.T) {
	task := identifiedTask(t)
	task.HIL.ProgramFamily = ""
	err := ValidateReviewedTask(task)
	if !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("an empty program family was accepted: %v", err)
	}
	if strings.Contains(err.Error(), "wider than") {
		t.Fatalf("the width door answered for an empty name: %v", err)
	}
}

// A task with no HIL contract has neither identifier, and this rule says
// nothing about it.
func TestANonHILTaskIsUntouchedByTheIdentifierRule(t *testing.T) {
	task := identifiedTask(t)
	task.HIL = nil
	if err := checkTheHILIdentifiersAreOnesARecordCanHold(task); err != nil {
		t.Fatalf("a task with no HIL contract was refused: %v", err)
	}
}

// The rule is an admission rule, not part of the runtime re-check: an agent
// holding such a task got it through a grant that already passed review, so
// ValidateTask must not retroactively refuse work already admitted.
func TestTheRuntimeRecheckDoesNotApplyTheIdentifierRule(t *testing.T) {
	task := identifiedTask(t)
	task.HIL.ProgramFamily = nameOfBytes(maxRecordableHILIdentifierBytes + 1)
	if err := ValidateTask(task); err != nil {
		t.Fatalf("the runtime re-check refused a width only review judges: %v", err)
	}
}

// A manifest carrying such a definition is refused at Load, the path a real
// catalog actually travels.
func TestAManifestNamingTooWideAnIdentifierIsRefused(t *testing.T) {
	task := identifiedTask(t)
	task.HIL.BoardID = nameOfBytes(maxRecordableHILIdentifierBytes + 1)
	if err := ValidateReviewedTask(task); !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("a manifest naming too wide a board id was accepted: %v", err)
	}
}
