// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
)

func plainTask() catalog.Task {
	return catalog.Task{Name: "build-firmware", Version: 1, Scope: "safe-local-read-only", BoardPolicy: "none"}
}

func hilContract() catalog.HILTask {
	return catalog.HILTask{
		BoardID: "bench-one", BoardModel: "EK-RA8D2", ManifestPath: "examples/ek_ra8d2/hil.conf",
		ProgramFamily: "alive", Mode: "alive", ObservationStep: "observe", FlashRestoreSeconds: 30,
	}
}

func hilTask() catalog.Task {
	contract := hilContract()
	return catalog.Task{Name: "hil-alive", Version: 1, Scope: "hil", BoardPolicy: "exclusive", HIL: &contract}
}

func TestATaskThatBindsNoArgumentsAcceptsAnEmptyArgv(t *testing.T) {
	persisted, err := checkedPersistedArguments([]byte(`{"argv":[]}`), plainTask())
	if err != nil {
		t.Fatalf("empty argv refused: %v", err)
	}
	if len(persisted.Args) != 0 || len(persisted.Values) != 0 || persisted.HIL != nil {
		t.Fatalf("decoded more than the row stated: %+v", persisted)
	}
}

// An absent argv is the same statement as an empty one, and rows written
// before the values key existed carry exactly that shape. The rule being
// tested is agreement with the schema, not the presence of a key.
func TestAnAbsentArgvIsReadAsNoArguments(t *testing.T) {
	if _, err := checkedPersistedArguments([]byte(`{}`), plainTask()); err != nil {
		t.Fatalf("absent argv refused: %v", err)
	}
}

func TestAnArgvNoBindingCouldProduceIsRefused(t *testing.T) {
	_, err := checkedPersistedArguments([]byte(`{"argv":["--target=all"]}`), plainTask())
	if err == nil {
		t.Fatal("an argv element the schema binds none of was accepted")
	}
	if !strings.Contains(err.Error(), "persisted") {
		t.Fatalf("refusal does not name the stored arguments: %v", err)
	}
}

func TestValuesForATaskDeclaringNoneAreRefused(t *testing.T) {
	if _, err := checkedPersistedArguments([]byte(`{"argv":[],"values":{"target":"all"}}`), plainTask()); err == nil {
		t.Fatal("named values for a task that declares none were accepted")
	}
}

// The guard this file adds, in the direction that used to go unread: a row
// may not carry board behaviour the reviewed definition does not declare.
func TestABoardContractOnANonBoardTaskIsRefused(t *testing.T) {
	row := `{"argv":[],"hil":{"board_id":"bench-one","board_model":"EK-RA8D2",` +
		`"manifest_path":"examples/ek_ra8d2/hil.conf","program_family":"alive","mode":"alive",` +
		`"observation_step":"observe","flash_restore_seconds":30,"timeout_declared":false}}`
	_, err := checkedPersistedArguments([]byte(row), plainTask())
	if err == nil {
		t.Fatal("a stored board contract was accepted for a task that declares none")
	}
	if !strings.Contains(err.Error(), "does not declare") {
		t.Fatalf("refusal does not say why: %v", err)
	}
}

func TestAHILTaskWhoseRowLostItsBoardContractIsRefused(t *testing.T) {
	if _, err := checkedPersistedArguments([]byte(`{"argv":[]}`), hilTask()); err == nil {
		t.Fatal("a HIL task with no stored board contract was accepted")
	}
}

func TestABoardContractThatDriftedFromReviewIsRefused(t *testing.T) {
	row := `{"argv":[],"hil":{"board_id":"bench-one","board_model":"EK-RA8D2",` +
		`"manifest_path":"examples/ek_ra8d2/other/hil.conf","program_family":"alive","mode":"alive",` +
		`"observation_step":"observe","flash_restore_seconds":30,"timeout_declared":false}}`
	_, err := checkedPersistedArguments([]byte(row), hilTask())
	if err == nil {
		t.Fatal("a board contract naming another manifest was accepted")
	}
	if !strings.Contains(err.Error(), "not its reviewed one") {
		t.Fatalf("refusal does not say why: %v", err)
	}
}

func TestAMatchingBoardContractIsAccepted(t *testing.T) {
	row := `{"argv":[],"hil":{"board_id":"bench-one","board_model":"EK-RA8D2",` +
		`"manifest_path":"examples/ek_ra8d2/hil.conf","program_family":"alive","mode":"alive",` +
		`"observation_step":"observe","flash_restore_seconds":30,"timeout_declared":false}}`
	persisted, err := checkedPersistedArguments([]byte(row), hilTask())
	if err != nil {
		t.Fatalf("the reviewed board contract was refused: %v", err)
	}
	if persisted.HIL == nil || *persisted.HIL != hilContract() {
		t.Fatalf("returned a contract other than the stored one: %+v", persisted.HIL)
	}
}

func TestAnUnknownStoredFieldIsRefused(t *testing.T) {
	if _, err := checkedPersistedArguments([]byte(`{"argv":[],"shell":"bash -c id"}`), plainTask()); err == nil {
		t.Fatal("a row carrying an unknown key was accepted")
	}
}

func TestTrailingStoredDataIsRefused(t *testing.T) {
	_, err := checkedPersistedArguments([]byte(`{"argv":[]}{"argv":["--x=1"]}`), plainTask())
	if err == nil {
		t.Fatal("a row carrying a second document was accepted")
	}
	if !strings.Contains(err.Error(), "trailing") {
		t.Fatalf("refusal does not name the trailing data: %v", err)
	}
}

func TestARowWithNoStoredArgumentsIsRefused(t *testing.T) {
	for _, raw := range [][]byte{nil, {}, []byte("   ")} {
		if _, err := checkedPersistedArguments(raw, plainTask()); err == nil {
			t.Fatalf("an empty arguments column was accepted (%q)", raw)
		}
	}
}

// The refusal names the task, because a claim refusing one row among many is
// read from a server log with no other handle on which row it was.
func TestARefusalNamesTheTask(t *testing.T) {
	_, err := checkedPersistedArguments(nil, plainTask())
	if err == nil || !strings.Contains(err.Error(), "build-firmware") {
		t.Fatalf("refusal does not name the task: %v", err)
	}
}
