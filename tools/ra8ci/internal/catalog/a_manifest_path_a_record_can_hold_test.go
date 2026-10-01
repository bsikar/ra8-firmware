package catalog

import (
	"errors"
	"strings"
	"testing"
)

// pathedTask is a reviewed HIL task this package already admits whole, so a
// refusal in these tests is the manifest-path door and nothing else.
func pathedTask(t *testing.T) Task {
	t.Helper()
	task := observedHILTask(t)
	if err := ValidateReviewedTask(task); err != nil {
		t.Fatalf("the fixture is not admitted before the rule is exercised: %v", err)
	}
	return task
}

func TestAManifestPathCarryingANULIsRefused(t *testing.T) {
	task := pathedTask(t)
	task.HIL.ManifestPath = "examples/ra8d2\x00probe/hil.conf"
	err := ValidateReviewedTask(task)
	if !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("a manifest path carrying a NUL was accepted: %v", err)
	}
	if !strings.Contains(err.Error(), task.Name) {
		t.Fatalf("refusal does not name the task: %v", err)
	}
}

func TestAManifestPathCarryingANewlineIsRefused(t *testing.T) {
	task := pathedTask(t)
	task.HIL.ManifestPath = "examples/ra8d2\nexamples/other/hil.conf"
	if err := ValidateReviewedTask(task); !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("a manifest path carrying a newline was accepted: %v", err)
	}
}

func TestAManifestPathCarryingAnEscapeSequenceIsRefused(t *testing.T) {
	task := pathedTask(t)
	task.HIL.ManifestPath = "examples/\x1b[31mra8d2/hil.conf"
	if err := ValidateReviewedTask(task); !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("a manifest path carrying an escape sequence was accepted: %v", err)
	}
}

// C1 is refused beside C0 for the board model's reason: a byte that renders as
// nothing is a byte no one can match by reading it, and these values are
// compared against rows already stored.
func TestAManifestPathCarryingAC1ControlIsRefused(t *testing.T) {
	task := pathedTask(t)
	task.HIL.ManifestPath = "examples/ra8d2\u0085probe/hil.conf"
	if err := ValidateReviewedTask(task); !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("a manifest path carrying a C1 control was accepted: %v", err)
	}
}

func TestAManifestPathCarryingADELIsRefused(t *testing.T) {
	task := pathedTask(t)
	task.HIL.ManifestPath = "examples/ra8d2\x7f/hil.conf"
	if err := ValidateReviewedTask(task); !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("a manifest path carrying a DEL was accepted: %v", err)
	}
}

func TestAManifestPathThatIsNotValidUTF8IsRefused(t *testing.T) {
	task := pathedTask(t)
	task.HIL.ManifestPath = "examples/ra8d2\xff/hil.conf"
	if err := ValidateReviewedTask(task); !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("a manifest path that is not valid UTF-8 was accepted: %v", err)
	}
}

// The shape rules already refused a path pointing out of the examples tree.
// This one adds nothing there, and the refusal a caller reads should stay the
// shape rule's, not this one's.
func TestThePathDoorLeavesTheShapeRulesAlone(t *testing.T) {
	task := pathedTask(t)
	task.HIL.ManifestPath = "examples/../secrets/hil.conf"
	err := ValidateReviewedTask(task)
	if !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("a traversing manifest path was accepted: %v", err)
	}
	if strings.Contains(err.Error(), "no record could hold") {
		t.Fatalf("the path door answered for a shape rule: %v", err)
	}
}

// A manifest path is a name in a repository tree, and this package has no
// standing to narrow that tree's alphabet past what a record requires.
func TestAPrintableNonASCIIManifestPathIsAdmitted(t *testing.T) {
	task := pathedTask(t)
	task.HIL.ManifestPath = "examples/ra8d2-café/hil.conf"
	if err := ValidateReviewedTask(task); err != nil {
		t.Fatalf("a printable non-ASCII manifest path was rejected: %v", err)
	}
}

// The door says nothing about a non-HIL task, which declares no manifest path
// at all.
func TestThePathDoorSaysNothingAboutANonHILTask(t *testing.T) {
	task := filableTask()
	if err := checkTheManifestPathIsOneARecordCanHold(task); err != nil {
		t.Fatalf("a non-HIL task was refused by the manifest path door: %v", err)
	}
}

// The embedded catalog every binary loads is admitted by the rule as it ships.
func TestTheEmbeddedCatalogPassesTheManifestPathDoor(t *testing.T) {
	loaded, err := Load()
	if err != nil {
		t.Fatalf("the embedded catalog no longer loads: %v", err)
	}
	for _, name := range loaded.Names() {
		task, _ := loaded.Task(name)
		if err := checkTheManifestPathIsOneARecordCanHold(task); err != nil {
			t.Fatalf("embedded task %q fails the manifest path door: %v", name, err)
		}
	}
}
