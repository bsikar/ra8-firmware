//go:build integration

package store

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/hilspec"
)

func TestIntegrationHILObservationHistoryUsesExactEvidenceBackedCohort(t *testing.T) {
	st, pool := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	boardID := "test-hil-" + mustID(t)
	leaseID := mustID(t)
	profileSHA := strings.Repeat("e", 64)
	leaseStart := time.Now().UTC().Add(-time.Minute)
	leaseExpiry := time.Now().UTC().Add(5 * time.Minute)
	if _, err := pool.Exec(ctx, `INSERT INTO boards (id,generation,state) VALUES ($1,1,'held')`, boardID); err != nil {
		t.Fatal(err)
	}
	if _, err := pool.Exec(ctx, `INSERT INTO board_leases
		(id,board_id,generation,holder_id,priority,reason,requested_duration_seconds,granted_at,
		 expires_at,state)
		VALUES ($1,$2,1,'hil-test','ci','integration HIL timing',300,$3,$4,'active')`,
		leaseID, boardID, leaseStart, leaseExpiry); err != nil {
		t.Fatal(err)
	}
	if _, err := pool.Exec(ctx, `INSERT INTO board_sessions
		(id,board_id,lease_id,owner_id,fixture_revision,profile_sha256,phase,restore_policy,
		 metadata_version,started_at)
		VALUES ($1,$2,$3,'hil-test','fixture-v1',$4,'held','restore-v1',1,$5)`,
		mustID(t), boardID, leaseID, profileSHA, leaseStart); err != nil {
		t.Fatal(err)
	}
	arguments := fmt.Sprintf(`{"argv":[],"hil":{"board_id":%q,"board_model":"EK-RA8D2","manifest_path":"examples/ek_ra8d2/hw_validated/hil/demo/hil.conf","program_family":"uart-demo","mode":"uart_scrape","observation_step":"observe","flash_restore_seconds":10}}`, boardID)
	in := CreateRunInput{Trigger: "integration", ActorID: "hil-test", Repository: "bsikar/ra8-firmware",
		CommitSHA: strings.Repeat("a", 40), SnapshotSHA256: strings.Repeat("b", 64),
		CatalogSHA256: strings.Repeat("c", 64), IdempotencyKey: "hil-" + mustID(t),
		RequestSHA256: strings.Repeat("d", 64),
		Tasks: []TaskInput{{Key: "hil-one", Name: "hil-run", Arguments: json.RawMessage(arguments),
			Tier: "required", Scope: "hil", HostClass: "hil-lab", DeadlineSeconds: 60}}}
	run, err := st.CreateRun(ctx, in)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := st.StartAttempt(ctx, testStart(run.Tasks[0].ID)); !errors.Is(err, ErrInvalid) {
		t.Fatalf("HIL task started without a board lease: %v", err)
	}
	startInput := testStart(run.Tasks[0].ID)
	startInput.BoardLeaseID = leaseID
	startInput.ActorID = "not-the-lease-holder"
	if _, err := st.StartAttempt(ctx, startInput); !errors.Is(err, ErrConflict) {
		t.Fatalf("HIL task accepted a foreign lease holder: %v", err)
	}
	startInput.ActorID = "hil-test"
	attempt, err := st.StartAttempt(ctx, startInput)
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
	sessionEnd := started.Add(duration + time.Second)
	if _, err := pool.Exec(ctx, `UPDATE board_sessions SET ended_at=$2 WHERE lease_id=$1`, leaseID, sessionEnd); err != nil {
		t.Fatal(err)
	}
	if _, err := pool.Exec(ctx, `UPDATE board_leases SET state='ended',ended_at=$2,end_reason='released'
		WHERE id=$1`, leaseID, sessionEnd); err != nil {
		t.Fatal(err)
	}
	workload := hilspec.Workload{ManifestPath: "examples/ek_ra8d2/hw_validated/hil/demo/hil.conf",
		BoardModel: "EK-RA8D2", FixtureRevision: "fixture-v1",
		ProfileSHA256: profileSHA, ProgramFamily: "uart-demo", Mode: hilspec.ModeUARTScrape}
	input := HILObservationInput{AttemptID: attempt.ID, ActorID: "hil-test", StepKey: "observe"}
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
