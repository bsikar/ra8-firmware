package boardagent

import (
	"context"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/hilspec"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

func TestReservedRecoveryInsideTheCeilingIsAccepted(t *testing.T) {
	for _, row := range []struct {
		margin  time.Duration
		restore time.Duration
	}{
		{0, time.Second},
		{time.Second, time.Second},
		{30 * time.Minute, 20 * time.Minute},
		{maxBoardOperation - time.Second, time.Second},
		{0, maxBoardOperation},
		{maxBoardOperation, 0},
	} {
		if err := checkReservedRecoveryIsOperable(row.margin, row.restore); err != nil {
			t.Fatalf("margin %s with restore bound %s was refused: %v", row.margin, row.restore, err)
		}
	}
}

func TestReservedRecoveryAboveTheCeilingIsRefused(t *testing.T) {
	for _, row := range []struct {
		margin  time.Duration
		restore time.Duration
	}{
		{maxBoardOperation, time.Nanosecond},
		{maxBoardOperation, time.Second},
		{maxBoardOperation, maxBoardOperation},
		{45 * time.Minute, 16 * time.Minute},
	} {
		err := checkReservedRecoveryIsOperable(row.margin, row.restore)
		if !errors.Is(err, ErrInvalidAgent) {
			t.Fatalf("margin %s with restore bound %s was accepted: %v", row.margin, row.restore, err)
		}
		if !strings.Contains(err.Error(), "observation step") {
			t.Fatalf("refusal does not name where the recovery is reserved: %v", err)
		}
	}
}

func TestANegativeRestoreBoundIsRefusedOnItsOwnTerms(t *testing.T) {
	err := checkReservedRecoveryIsOperable(0, -time.Second)
	if !errors.Is(err, ErrInvalidAgent) {
		t.Fatalf("a negative flash restore bound was accepted: %v", err)
	}
	if !strings.Contains(err.Error(), "negative flash restore bound") {
		t.Fatalf("refusal does not say what was wrong with the bound: %v", err)
	}
}

// The sum is judged here precisely so RunSegment never has to. If the two ever
// disagree about the ceiling, the refusal moves back into the middle of an
// attempt, which is the whole point of asking early.
func TestTheEarlyCheckAppliesExactlyTheCeilingTheSegmentDoorApplies(t *testing.T) {
	agent, client, token := newActiveSegmentAgent(t)
	for _, margin := range []time.Duration{0, time.Second, 30 * time.Minute,
		maxBoardOperation - time.Second, maxBoardOperation, maxBoardOperation + time.Second, 2 * maxBoardOperation} {
		early := checkReservedRecoveryIsOperable(margin, 0) != nil
		begins := client.begins
		_, segmentErr := agent.RunSegment(context.Background(), token, "01996f90-3415-7cfe-8ff1-600058131aff",
			"ceiling-cross", time.Millisecond, margin, func(context.Context) error { return nil })
		door := errors.Is(segmentErr, ErrInvalidAgent) && client.begins == begins
		if early != door {
			t.Fatalf("margin %s: early check refuses=%v, segment door refuses=%v (err %v)",
				margin, early, door, segmentErr)
		}
	}
}

func hilAttemptRoot(t *testing.T) string {
	t.Helper()
	root := t.TempDir()
	manifest := filepath.Join(root, "examples", "test", "hil.conf")
	if err := os.MkdirAll(filepath.Dir(manifest), 0o700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(manifest, []byte("HIL_MODE=uart_scrape\nHIL_TIMEOUT_S=12\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	return root
}

func hilAttemptWorkload() hilspec.Workload {
	return hilspec.Workload{ManifestPath: "examples/test/hil.conf", BoardModel: "EK-RA8D2",
		FixtureRevision: "fixture-v2", ProfileSHA256: "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee",
		ProgramFamily: "uart-demo", Mode: hilspec.ModeUARTScrape}
}

// The observation step is never the first step here, which is the ordinary
// shape: a board is flashed, then watched.
func hilAttemptTask(boardID string) catalog.Task {
	workload := hilAttemptWorkload()
	return catalog.Task{Name: "uart-demo", Version: 1, Tier: "required", Scope: "hil",
		OS: []string{"linux"}, DeadlineSeconds: 20, BoardPolicy: "exclusive",
		Retry: catalog.RetryPolicy{MaxAttempts: 1},
		Steps: []catalog.Step{{Name: "flash", Program: "ra8ci:hil-flash"},
			{Name: "observe", Program: "ra8ci:hil-observe"}},
		HIL: &catalog.HILTask{BoardID: boardID, BoardModel: workload.BoardModel,
			ManifestPath: workload.ManifestPath, ProgramFamily: workload.ProgramFamily,
			Mode: string(workload.Mode), ObservationStep: "observe", FlashRestoreSeconds: 10,
			TimeoutDeclared: true, TimeoutSeconds: 12}}
}

func hilAttemptAssignment(boardID string) store.BoardHILAssignment {
	task := hilAttemptTask(boardID)
	started := time.Now().UTC()
	return store.BoardHILAssignment{
		Attempt: store.Attempt{ID: "01996f90-3415-7cfe-8ff1-600058131aff",
			TaskID: "01996f90-3415-7cfe-8ff1-600058131b11", AttemptNo: 1, State: "running",
			StartedAt: started, DeadlineAt: started.Add(time.Minute)},
		Task: task, CatalogSHA256: "reviewed-catalog",
		HILTiming: &store.HILTimingEvidence{Workload: hilAttemptWorkload(),
			Decision: hilspec.Decision{ValidityWindow: 12 * time.Second, Source: "hil.conf",
				FlashRestoreBound: 10 * time.Second}}}
}

// A board is flashed before it is watched, so a refusal that waits for the
// observation step is a refusal delivered with the fixture already programmed.
func TestAnUnreservableRecoveryStopsTheAttemptBeforeAnyStepRuns(t *testing.T) {
	agent, client, token := newActiveSegmentAgent(t)
	assignment := hilAttemptAssignment(token.BoardID)
	if err := catalog.ValidateTask(assignment.Task); err != nil {
		t.Fatalf("test task invalid: %v", err)
	}
	steps := 0
	completion, err := agent.RunHILAttempt(context.Background(), token, hilAttemptRoot(t), assignment,
		20*time.Second, maxBoardOperation, func(context.Context, string, catalog.Task, catalog.Step) (int, error) {
			steps++
			return 0, nil
		})
	if err != nil {
		t.Fatalf("terminal evidence was not persisted: %v", err)
	}
	if steps != 0 || client.begins != 0 {
		t.Fatalf("the attempt reached the board before its recovery was judged: steps=%d begins=%d", steps, client.begins)
	}
	if completion.Result != "failed" || completion.EvidenceComplete {
		t.Fatalf("unreservable recovery did not fail the attempt: %+v", completion)
	}
	if !strings.Contains(completion.Reason, "observation step") {
		t.Fatalf("recorded reason does not name the reserved recovery: %q", completion.Reason)
	}
}

// The margin a caller passes is ordinary on its own; only the sum is not, so
// this pins that the check does not start refusing plain attempts.
func TestAnOrdinaryRecoveryStillRunsTheAttempt(t *testing.T) {
	agent, client, token := newActiveSegmentAgent(t)
	assignment := hilAttemptAssignment(token.BoardID)
	steps := 0
	completion, err := agent.RunHILAttempt(context.Background(), token, hilAttemptRoot(t), assignment,
		20*time.Second, time.Second, func(context.Context, string, catalog.Task, catalog.Step) (int, error) {
			steps++
			return 0, nil
		})
	if err != nil {
		t.Fatalf("terminal evidence was not persisted: %v", err)
	}
	if strings.Contains(completion.Reason, "observation step") {
		t.Fatalf("an ordinary recovery margin was refused as unreservable: %q", completion.Reason)
	}
	if steps != len(assignment.Task.Steps) || client.begins != len(assignment.Task.Steps) {
		t.Fatalf("an ordinary attempt did not run its steps: steps=%d begins=%d", steps, client.begins)
	}
}
