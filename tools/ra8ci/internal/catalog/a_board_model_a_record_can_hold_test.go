package catalog

import (
	"errors"
	"strings"
	"testing"
)

// boardedTask is a reviewed HIL task this package already admits whole, so a
// refusal in these tests is the board-model door and nothing else.
func boardedTask(t *testing.T) Task {
	t.Helper()
	task := observedHILTask(t)
	if err := ValidateReviewedTask(task); err != nil {
		t.Fatalf("the fixture is not admitted before the rule is exercised: %v", err)
	}
	return task
}

func TestABoardModelCarryingANewlineIsRefused(t *testing.T) {
	task := boardedTask(t)
	task.HIL.BoardModel = "EK-RA8D2\nEK-RA8D3"
	err := ValidateReviewedTask(task)
	if !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("a board model carrying a newline was accepted: %v", err)
	}
	if !strings.Contains(err.Error(), task.Name) {
		t.Fatalf("refusal does not name the task: %v", err)
	}
}

func TestABoardModelCarryingANULIsRefused(t *testing.T) {
	task := boardedTask(t)
	task.HIL.BoardModel = "EK-RA8D2\x00"
	if err := ValidateReviewedTask(task); !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("a board model carrying a NUL was accepted: %v", err)
	}
}

func TestABoardModelCarryingAnEscapeSequenceIsRefused(t *testing.T) {
	task := boardedTask(t)
	task.HIL.BoardModel = "EK-\x1b[31mRA8D2"
	if err := ValidateReviewedTask(task); !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("a board model carrying an escape sequence was accepted: %v", err)
	}
}

// C1 is refused beside C0: U+0085 is a line break to a terminal and renders as
// nothing to a reader matching one model against another.
func TestABoardModelCarryingAC1ControlIsRefused(t *testing.T) {
	task := boardedTask(t)
	task.HIL.BoardModel = "EK-RA8D2\u0085next"
	if err := ValidateReviewedTask(task); !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("a board model carrying a C1 control was accepted: %v", err)
	}
}

func TestABoardModelThatIsNotValidUTF8IsRefused(t *testing.T) {
	task := boardedTask(t)
	task.HIL.BoardModel = "EK-RA8D2-\xff"
	if err := ValidateReviewedTask(task); !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("a board model that is not valid UTF-8 was accepted: %v", err)
	}
}

// The door states no alphabet: a vendor spells its own hardware, and a model
// carrying a trademark sign or a non-ASCII letter is text a record holds.
func TestABoardModelMayCarryNonASCIIText(t *testing.T) {
	for _, model := range []string{"EK-RA8D2\u2122", "EK-RA8D2 (Revision Grr\u00f6\u00dfe)", "RA8 \u30dc\u30fc\u30c9"} {
		task := boardedTask(t)
		task.HIL.BoardModel = model
		if err := ValidateReviewedTask(task); err != nil {
			t.Fatalf("board model %q was rejected: %v", model, err)
		}
	}
}

func TestAnOrdinaryBoardModelIsAdmitted(t *testing.T) {
	task := boardedTask(t)
	task.HIL.BoardModel = "EK-RA8D2"
	if err := ValidateReviewedTask(task); err != nil {
		t.Fatalf("an ordinary board model was rejected: %v", err)
	}
}

// The door judges the text and nothing else: the length and whitespace bounds
// this field already had are ValidateHILTaskMetadata's and still hold.
func TestTheBoardModelDoorDoesNotReplaceTheBoundsItSitsBeside(t *testing.T) {
	wide := boardedTask(t)
	wide.HIL.BoardModel = strings.Repeat("m", 129)
	if err := ValidateReviewedTask(wide); !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("a board model over 128 bytes was accepted: %v", err)
	}
	spaced := boardedTask(t)
	spaced.HIL.BoardModel = " EK-RA8D2 "
	if err := ValidateReviewedTask(spaced); !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("a board model carrying surrounding whitespace was accepted: %v", err)
	}
	empty := boardedTask(t)
	empty.HIL.BoardModel = ""
	if err := ValidateReviewedTask(empty); !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("an empty board model was accepted: %v", err)
	}
}

// A task declaring no board declares no board model, and the door says
// nothing about it.
func TestTheBoardModelDoorSaysNothingAboutATaskWithNoBoard(t *testing.T) {
	loaded, err := Load()
	if err != nil {
		t.Fatal(err)
	}
	task, found := loaded.Task("format")
	if !found {
		t.Fatal("format fixture not found")
	}
	if task.HIL != nil {
		t.Fatalf("the non-HIL fixture declares a board: %+v", task.HIL)
	}
	if err := checkTheBoardModelIsOneARecordCanHold(task); err != nil {
		t.Fatalf("a task with no board was refused by the board-model door: %v", err)
	}
}

// The rule is stated about the catalog every ra8ci binary carries, so the
// catalog has to satisfy it.
func TestTheEmbeddedCatalogSatisfiesTheBoardModelDoor(t *testing.T) {
	loaded, err := Load()
	if err != nil {
		t.Fatal(err)
	}
	boards := 0
	for _, name := range loaded.Names() {
		task, found := loaded.Task(name)
		if !found {
			t.Fatalf("catalog lists %q and does not carry it", name)
		}
		if task.HIL == nil {
			continue
		}
		boards++
		if err := checkTheBoardModelIsOneARecordCanHold(task); err != nil {
			t.Fatalf("embedded task %q: %v", name, err)
		}
	}
	t.Logf("checked %d embedded HIL task(s)", boards)
}
