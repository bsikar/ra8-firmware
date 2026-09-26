// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
)

// argumentTask is a read-only task that declares one positional and one flag,
// the shape the dispatcher hands to an outbound agent. The v1 catalog has no
// such task, which is why the gap this file pins is not visible from it.
func argumentTask() catalog.Task {
	task := plainTask()
	task.ArgsSchema = catalog.ArgsSchema{Positional: []string{"path"}, Flags: []string{"profile"}}
	return task
}

func TestATaskCarryingNoArgumentsIsAssignable(t *testing.T) {
	persisted, err := checkedAssignableArguments([]byte(`{"argv":[]}`), plainTask())
	if err != nil {
		t.Fatalf("an argument-free row was refused an assignment: %v", err)
	}
	if len(persisted.Args) != 0 || len(persisted.Values) != 0 {
		t.Fatalf("decoded more than the row stated: %+v", persisted)
	}
}

// Every task in the v1 catalog is this case: no schema, no values, no argv.
func TestAnAbsentArgvIsAssignable(t *testing.T) {
	if _, err := checkedAssignableArguments([]byte(`{}`), plainTask()); err != nil {
		t.Fatalf("a row written before the values key existed was refused: %v", err)
	}
}

// The gap this slice closes. The row is internally consistent and would pass
// the persisted-arguments check, but protocol.Assignment has nowhere to put
// these two elements, so the agent would run the reviewed steps alone.
func TestABoundPositionalIsNotAssignable(t *testing.T) {
	row := `{"argv":["src/main.c","--profile=release"],"values":{"path":"src/main.c","profile":"release"}}`
	if _, err := checkedPersistedArguments([]byte(row), argumentTask()); err != nil {
		t.Fatalf("fixture is not a valid persisted row, the test would prove nothing: %v", err)
	}
	_, err := checkedAssignableArguments([]byte(row), argumentTask())
	if err == nil {
		t.Fatal("a row carrying bound arguments was issued to an agent that cannot receive them")
	}
	if !strings.Contains(err.Error(), "outbound assignment carries none") ||
		!strings.Contains(err.Error(), "2 bound argument") {
		t.Fatalf("refusal does not name the carrier or the count: %v", err)
	}
}

// The quiet half of the gap, and the reason this is not merely untidy: a
// flags-only schema binds to an argv the agent silently drops, so the task
// runs without the requested flag and reports success against a row that says
// it ran with it. Nothing fails, which is worse than a missing positional.
func TestAFlagsOnlyRowIsNotAssignableEither(t *testing.T) {
	task := plainTask()
	task.ArgsSchema = catalog.ArgsSchema{Flags: []string{"profile"}}
	row := `{"argv":["--profile=release"],"values":{"profile":"release"}}`
	if _, err := checkedPersistedArguments([]byte(row), task); err != nil {
		t.Fatalf("fixture is not a valid persisted row: %v", err)
	}
	if _, err := checkedAssignableArguments([]byte(row), task); err == nil {
		t.Fatal("a row carrying only optional flags was issued to an agent that drops them")
	}
}

// Values with no argv can only be a row nothing bound, but it is still a row
// stating arguments an assignment cannot carry, so it is refused here too
// rather than issued on the grounds that the argv happened to be empty.
func TestStoredValuesWithNoArgvAreNotAssignable(t *testing.T) {
	row := `{"argv":[],"values":{"path":"src/main.c"}}`
	if _, err := checkedAssignableArguments([]byte(row), argumentTask()); err == nil {
		t.Fatal("a row stating values was issued as an argument-free assignment")
	}
}

// This function only ADDS a refusal. Everything checkedPersistedArguments
// refuses it must still refuse, with that function's own reason, so a
// dispatcher error still names what is actually wrong with the row.
func TestTheCarrierRuleDoesNotReplaceTheRowRules(t *testing.T) {
	for _, row := range []string{
		`{"argv":["--target=all"]}`,
		`{"argv":[],"shell":"bash -c id"}`,
		`{"argv":[]}{"argv":[]}`,
		``,
	} {
		if _, err := checkedAssignableArguments([]byte(row), plainTask()); err == nil {
			t.Fatalf("row %q was accepted as assignable", row)
		}
	}
}

// A HIL task is not dispatched through protocol.Assignment at all: its
// arguments travel in BoardHILAssignment.Args and are re-checked by the board
// client on arrival. This pins that the refusal is a property of the outbound
// carrier, so the board path keeps reading rows through
// checkedPersistedArguments and is untouched by this change.
func TestTheBoardPathStillReadsArgumentsThroughTheRowRules(t *testing.T) {
	contract := hilContract()
	row := `{"argv":[],"hil":{"board_id":"bench-one","board_model":"EK-RA8D2",` +
		`"manifest_path":"examples/ek_ra8d2/hil.conf","program_family":"alive","mode":"alive",` +
		`"observation_step":"observe","flash_restore_seconds":30,"timeout_declared":false}}`
	persisted, err := checkedPersistedArguments([]byte(row), hilTask())
	if err != nil {
		t.Fatalf("a valid HIL row was refused: %v", err)
	}
	if persisted.HIL == nil || *persisted.HIL != contract {
		t.Fatalf("HIL contract did not survive the read: %+v", persisted.HIL)
	}
}
