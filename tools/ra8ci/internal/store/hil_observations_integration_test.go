//go:build integration

package store

import (
	"context"
	"encoding/json"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/hilspec"
)

func TestIntegrationHILObservationHistoryUsesExactEvidenceBackedCohort(t *testing.T) {
	st, _ := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	in := CreateRunInput{Trigger: "integration", ActorID: "hil-test", Repository: "bsikar/ra8-firmware",
		CommitSHA: strings.Repeat("a", 40), SnapshotSHA256: strings.Repeat("b", 64),
		CatalogSHA256: strings.Repeat("c", 64), IdempotencyKey: "hil-" + mustID(t),
		RequestSHA256: strings.Repeat("d", 64),
		Tasks: []TaskInput{{Key: "hil-one", Name: "hil-run", Arguments: json.RawMessage(`{}`),
			Tier: "required", Scope: "hil", HostClass: "hil-lab", DeadlineSeconds: 60}}}
	run, err := st.CreateRun(ctx, in)
	if err != nil {
		t.Fatal(err)
	}
	attempt, err := st.StartAttempt(ctx, testStart(run.Tasks[0].ID))
	if err != nil {
		t.Fatal(err)
	}
	started := time.Now().UTC()
	duration := 8 * time.Second
	zero := 0
	if err := st.RecordStep(ctx, StepInput{AttemptID: attempt.ID, ActorID: "hil-test",
		Key: "observe", Ordinal: 0, Phase: "hil_observe", StartedAt: started,
		EndedAt: started.Add(duration), DurationNS: duration.Nanoseconds(),
		State: "succeeded", ChildExitCode: &zero}); err != nil {
		t.Fatal(err)
	}
	if err := st.FinishAttempt(ctx, FinishAttemptInput{AttemptID: attempt.ID, ActorID: "hil-test",
		Result: "succeeded", ChildExitCode: &zero, EvidenceComplete: true}); err != nil {
		t.Fatal(err)
	}
	workload := hilspec.Workload{ManifestPath: "examples/ek_ra8d2/hw_validated/hil/demo/hil.conf",
		BoardModel: "EK-RA8D2", FixtureRevision: "fixture-v1",
		ProfileSHA256: strings.Repeat("e", 64), ProgramFamily: "uart-demo", Mode: hilspec.ModeUARTScrape}
	input := HILObservationInput{AttemptID: attempt.ID, ActorID: "hil-test", StepKey: "observe", Workload: workload}
	if err := st.RecordHILObservation(ctx, input); err != nil {
		t.Fatal(err)
	}
	if err := st.RecordHILObservation(ctx, input); err != nil {
		t.Fatalf("idempotent observation retry: %v", err)
	}
	rows, err := st.Observations(ctx, workload)
	if err != nil || len(rows) != 1 {
		t.Fatalf("history rows=%+v err=%v", rows, err)
	}
	got := rows[0]
	if got.Workload != workload || got.Duration != duration || !got.Succeeded ||
		!got.EvidenceComplete || got.TimedOut {
		t.Fatalf("incorrect HIL history row: %+v", got)
	}
	other := workload
	other.BoardModel = "different-board"
	rows, err = st.Observations(ctx, other)
	if err != nil || len(rows) != 0 {
		t.Fatalf("cross-cohort history leaked: rows=%+v err=%v", rows, err)
	}
}
