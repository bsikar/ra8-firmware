package catalog

import (
	"errors"
	"testing"
	"time"
)

func handoffHILTask(t *testing.T) Task {
	t.Helper()
	loaded, err := Load()
	if err != nil {
		t.Fatal(err)
	}
	task, found := loaded.Task("format")
	if !found {
		t.Fatal("format fixture not found")
	}
	task.Scope = "hil"
	task.BoardPolicy = "exclusive"
	task.Steps = append(task.Steps, Step{Name: "observe", Program: "ra8ci", Args: []string{"internal-observe"}})
	task.HIL = &HILTask{BoardID: "ek-ra8d2", BoardModel: "EK-RA8D2",
		ManifestPath:  "examples/ek_ra8d2/hw_validated/hil/demo/hil.conf",
		ProgramFamily: "uart-demo", Mode: "uart_scrape", ObservationStep: "observe", FlashRestoreSeconds: 10}
	return task
}

// Undeclared is a real answer, not a gap to fill in with a default: a task
// with no declared bounds has an unknown handoff ETA, and the estimator
// refuses to invent one.
func TestHandoffBoundsMayGoUndeclared(t *testing.T) {
	task := handoffHILTask(t)
	if err := ValidateTask(task); err != nil {
		t.Fatalf("a task declaring no handoff bounds was rejected: %v", err)
	}
	if task.HIL.HandoffBoundsDeclared() || task.HIL.HandoffSafetyBound() != 0 {
		t.Fatalf("undeclared bounds reported as declared: %+v", task.HIL)
	}
}

func TestDeclaredHandoffBoundsSumToTheSafetyBound(t *testing.T) {
	task := handoffHILTask(t)
	task.HIL.HandoffSafeStepSeconds = 12
	task.HIL.HandoffRestoreProbeSeconds = 8
	if err := ValidateTask(task); err != nil {
		t.Fatalf("valid handoff bounds rejected: %v", err)
	}
	if !task.HIL.HandoffBoundsDeclared() {
		t.Fatal("declared bounds reported as undeclared")
	}
	if got := task.HIL.HandoffSafetyBound(); got != 20*time.Second {
		t.Fatalf("safety bound %s, want 20s", got)
	}
}

// Half a bound is worse than none: the sum is what an ETA is floored by, so
// a task stating one half would quote a target that omits work it always
// pays, or assume it can be interrupted anywhere.
func TestHalfDeclaredHandoffBoundsAreRefused(t *testing.T) {
	for index, edit := range []func(*HILTask){
		func(hil *HILTask) { hil.HandoffSafeStepSeconds = 12 },
		func(hil *HILTask) { hil.HandoffRestoreProbeSeconds = 8 },
	} {
		task := handoffHILTask(t)
		edit(task.HIL)
		if err := ValidateTask(task); !errors.Is(err, ErrInvalidCatalog) {
			t.Fatalf("half-declared bound %d accepted: %v", index, err)
		}
		if task.HIL.HandoffBoundsDeclared() {
			t.Fatalf("half-declared bound %d reported as declared", index)
		}
	}
}

func TestHandoffBoundsStayInsideTheEstimatorsCeiling(t *testing.T) {
	for index, edit := range []func(*HILTask){
		func(hil *HILTask) {
			hil.HandoffSafeStepSeconds, hil.HandoffRestoreProbeSeconds = maxHandoffBoundSeconds+1, 8
		},
		func(hil *HILTask) {
			hil.HandoffSafeStepSeconds, hil.HandoffRestoreProbeSeconds = 12, maxHandoffBoundSeconds+1
		},
		func(hil *HILTask) { hil.HandoffSafeStepSeconds, hil.HandoffRestoreProbeSeconds = -1, 8 },
		func(hil *HILTask) { hil.HandoffSafeStepSeconds, hil.HandoffRestoreProbeSeconds = 12, -1 },
	} {
		task := handoffHILTask(t)
		edit(task.HIL)
		if err := ValidateTask(task); !errors.Is(err, ErrInvalidCatalog) {
			t.Fatalf("out-of-range bound %d accepted: %v", index, err)
		}
	}
	task := handoffHILTask(t)
	task.HIL.HandoffSafeStepSeconds = maxHandoffBoundSeconds
	task.HIL.HandoffRestoreProbeSeconds = maxHandoffBoundSeconds
	if err := ValidateTask(task); err != nil {
		t.Fatalf("a task declaring the ceiling twice was rejected: %v", err)
	}
}

// An indivisible step cannot outlast the cap on the attempt it runs inside.
func TestASafeStepMayNotOutlastTheAttemptSafetyMaximum(t *testing.T) {
	task := handoffHILTask(t)
	task.HIL.TimeoutDeclared = true
	task.HIL.TimeoutSeconds = 60
	task.HIL.SafetyMaximumSeconds = 120
	task.HIL.HandoffRestoreProbeSeconds = 8

	task.HIL.HandoffSafeStepSeconds = 121
	if err := ValidateTask(task); !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("a safe step longer than the attempt cap was accepted: %v", err)
	}
	task.HIL.HandoffSafeStepSeconds = 120
	if err := ValidateTask(task); err != nil {
		t.Fatalf("a safe step exactly at the attempt cap was rejected: %v", err)
	}
}

// The catalog is the only source of these numbers, so an unreviewed manifest
// cannot introduce them and a reviewed one cannot carry a half pair.
func TestManifestHandoffBoundsGoThroughTheSameValidation(t *testing.T) {
	task := handoffHILTask(t)
	task.HIL.HandoffSafeStepSeconds = 12
	task.HIL.HandoffRestoreProbeSeconds = 8
	clone := cloneTask(task)
	clone.HIL.HandoffSafeStepSeconds = 999999
	if task.HIL.HandoffSafeStepSeconds != 12 {
		t.Fatal("task accessor exposed mutable handoff bounds")
	}
	if err := ValidateTask(clone); !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("mutated clone accepted: %v", err)
	}
}
