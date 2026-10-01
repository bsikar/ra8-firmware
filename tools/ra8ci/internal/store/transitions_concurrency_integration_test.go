//go:build integration

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/protocol"
	"github.com/jackc/pgx/v5/pgxpool"
)

// The unit tests state what the machines in transitions.go allow. These state
// what concurrent writers do to the rows those machines describe: the claim,
// the reaper, the acknowledgment and cancellation all race for the same task
// and attempt, and every one of them writes fenced on the state it checked.
// A lost fence shows up here as two attempts on one task, a cancelled task
// with a live attempt, or an attempt reaped twice, not as a failed unit test.

type concurrencyFixture struct {
	store      *Store
	pool       *pgxpool.Pool
	catalog    *catalog.Catalog
	definition catalog.Task
	facts      protocol.HostFacts
	repository string
	commit     string
}

// claimableDefinition is the catalog's own answer to "what can a linux agent
// be handed": the claim filters on exactly these properties, so the fixture
// reads them out of the catalog instead of naming a task the manifest may
// rename or retire.
func claimableDefinition(t *testing.T, definitions *catalog.Catalog) catalog.Task {
	t.Helper()
	var chosen catalog.Task
	for _, name := range definitions.Names() {
		definition, found := definitions.Task(name)
		if !found || definition.Scope != "safe-local-read-only" ||
			definition.BoardPolicy != "none" || len(definition.Steps) == 0 ||
			!definition.SupportsOS("linux") {
			continue
		}
		if chosen.Name == "" || name == "format-check" {
			chosen = definition
		}
	}
	if chosen.Name == "" {
		t.Fatal("catalog has no safe-local-read-only linux task to dispatch")
	}
	return chosen
}

func newConcurrencyFixture(t *testing.T) *concurrencyFixture {
	t.Helper()
	st, pool := integrationStore(t)
	definitions, err := catalog.Load()
	if err != nil {
		t.Fatal(err)
	}
	return &concurrencyFixture{store: st, pool: pool, catalog: definitions,
		definition: claimableDefinition(t, definitions),
		repository: "bsikar/ra8ci-concurrency-test-" + mustID(t),
		commit:     strings.Repeat("a", 40),
		facts: protocol.HostFacts{Cores: 4, RAMBytes: 8 << 30, RAMFreeBytes: 4 << 30,
			Load1: 0.5, LoadKind: "linux_load1", OS: "linux", Arch: "amd64",
			CapturedAt: time.Now().UTC()}}
}

// agent enrolls one healthy linux agent holding an executor grant on the
// fixture repository and returns the certificate it authenticates with.
func (f *concurrencyFixture) agent(t *testing.T) []byte {
	t.Helper()
	ctx := context.Background()
	cert := []byte("concurrency-agent-" + mustID(t))
	fingerprint := sha256.Sum256(cert)
	principal := "concurrency-agent-" + mustID(t)
	agentID := mustID(t)
	_, err := f.pool.Exec(ctx, `INSERT INTO api_principals
		(cert_sha256,principal_id,kind,expires_at)
		VALUES ($1,$2,'agent',clock_timestamp()+interval '1 hour')`,
		hex.EncodeToString(fingerprint[:]), principal)
	if err != nil {
		t.Fatal(err)
	}
	_, err = f.pool.Exec(ctx, `INSERT INTO api_grants(principal_id,repository,role)
		VALUES ($1,$2,'agent_executor')`, principal, f.repository)
	if err != nil {
		t.Fatal(err)
	}
	_, err = f.pool.Exec(ctx, `INSERT INTO agents(id,principal_id,host_class,version,
		capabilities,capacity,state) VALUES ($1,$2,'linux-vm','test',
		'{"os":"linux"}'::jsonb,1,'healthy')`, agentID, principal)
	if err != nil {
		t.Fatal(err)
	}
	return cert
}

func (f *concurrencyFixture) run(t *testing.T, tasks int) Run {
	t.Helper()
	in := CreateRunInput{Trigger: "integration", ActorID: "concurrency-submitter",
		Repository: f.repository, Branch: "test", CommitSHA: f.commit,
		SnapshotSHA256: strings.Repeat("b", 64), CatalogSHA256: f.catalog.Digest()}
	for ordinal := 0; ordinal < tasks; ordinal++ {
		in.Tasks = append(in.Tasks, TaskInput{Key: "task-" + mustID(t),
			Name: f.definition.Name, Arguments: json.RawMessage(`{"argv":[]}`),
			Tier: "required", Scope: "safe-local-read-only",
			HostClass: "safe-local-read-only", DeadlineSeconds: f.definition.DeadlineSeconds})
	}
	run, err := f.store.CreateRun(context.Background(), in)
	if err != nil {
		t.Fatal(err)
	}
	if len(run.Tasks) != tasks {
		t.Fatalf("run admitted %d tasks, want %d", len(run.Tasks), tasks)
	}
	return run
}

// expire moves an attempt's deadline far enough into the past that the reaper
// grace has also elapsed, which is what the reaper actually tests against.
func (f *concurrencyFixture) expire(t *testing.T, attemptID string) {
	t.Helper()
	_, err := f.pool.Exec(context.Background(), `UPDATE task_attempts
		SET deadline_at=clock_timestamp()-($2 * interval '1 second') WHERE id=$1`,
		attemptID, int(agentReaperGrace.Seconds())+120)
	if err != nil {
		t.Fatal(err)
	}
}

type taskRow struct {
	state    string
	attempts int
}

func (f *concurrencyFixture) tasksOf(t *testing.T, runID string) map[string]taskRow {
	t.Helper()
	rows, err := f.pool.Query(context.Background(), `SELECT t.id::text, t.state,
		COUNT(a.id)::int FROM tasks t LEFT JOIN task_attempts a ON a.task_id=t.id
		WHERE t.run_id=$1 GROUP BY t.id, t.state`, runID)
	if err != nil {
		t.Fatal(err)
	}
	defer rows.Close()
	states := map[string]taskRow{}
	for rows.Next() {
		var id string
		var row taskRow
		if err := rows.Scan(&id, &row.state, &row.attempts); err != nil {
			t.Fatal(err)
		}
		states[id] = row
	}
	if err := rows.Err(); err != nil {
		t.Fatal(err)
	}
	return states
}

func (f *concurrencyFixture) attemptState(t *testing.T, attemptID string) string {
	t.Helper()
	var state string
	if err := f.pool.QueryRow(context.Background(),
		"SELECT state FROM task_attempts WHERE id=$1", attemptID).Scan(&state); err != nil {
		t.Fatal(err)
	}
	return state
}

// TestIntegrationConcurrentClaimsAssignEachTaskOnce races more agents than
// there is work. Every claim writes scheduled -> running behind a fence, so
// the count of started tasks has to equal the count of issued assignments and
// no task may carry two attempts.
func TestIntegrationConcurrentClaimsAssignEachTaskOnce(t *testing.T) {
	const tasks, agents = 4, 8
	f := newConcurrencyFixture(t)
	certs := make([][]byte, agents)
	for i := range certs {
		certs[i] = f.agent(t)
	}
	run := f.run(t, tasks)
	ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
	defer cancel()
	grants := make([]*protocol.Assignment, agents)
	failures := make([]error, agents)
	var start sync.WaitGroup
	var done sync.WaitGroup
	start.Add(1)
	for i := range certs {
		done.Add(1)
		go func(i int) {
			defer done.Done()
			start.Wait()
			grants[i], failures[i] = f.store.ClaimAgentTask(ctx, certs[i], f.facts, f.catalog, f.commit)
		}(i)
	}
	start.Done()
	done.Wait()
	issued := map[string]bool{}
	for i, err := range failures {
		if err != nil {
			t.Fatalf("agent %d claim failed: %v", i, err)
		}
		if grants[i] == nil {
			continue
		}
		if issued[grants[i].AttemptID] {
			t.Fatalf("attempt %s issued to two agents", grants[i].AttemptID)
		}
		issued[grants[i].AttemptID] = true
	}
	if len(issued) != tasks {
		t.Fatalf("issued %d assignments for %d claimable tasks", len(issued), tasks)
	}
	running := 0
	for id, row := range f.tasksOf(t, run.ID) {
		if row.attempts > 1 {
			t.Fatalf("task %s carries %d attempts", id, row.attempts)
		}
		switch row.state {
		case "running":
			running++
			if row.attempts != 1 {
				t.Fatalf("running task %s has %d attempts", id, row.attempts)
			}
		case "scheduled":
			if row.attempts != 0 {
				t.Fatalf("scheduled task %s already has %d attempts", id, row.attempts)
			}
		default:
			t.Fatalf("task %s reached %q from a claim race", id, row.state)
		}
	}
	if running != len(issued) {
		t.Fatalf("%d tasks started for %d assignments", running, len(issued))
	}
	var state string
	if err := f.pool.QueryRow(ctx, "SELECT state FROM runs WHERE id=$1", run.ID).Scan(&state); err != nil {
		t.Fatal(err)
	}
	if state != "running" {
		t.Fatalf("run state after concurrent claims is %q", state)
	}
}

// TestIntegrationConcurrentAckAndReaperPickOneWinner races the agent's
// acknowledgment against the reaper on an attempt whose deadline has already
// passed. One of them writes; the loser has to see a conflict rather than
// overwrite a terminal row, and the attempt has to land on a state the
// machine allows out of issued.
func TestIntegrationConcurrentAckAndReaperPickOneWinner(t *testing.T) {
	for round := 0; round < 4; round++ {
		f := newConcurrencyFixture(t)
		cert := f.agent(t)
		f.run(t, 1)
		ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
		grant, err := f.store.ClaimAgentTask(ctx, cert, f.facts, f.catalog, f.commit)
		if err != nil || grant == nil {
			cancel()
			t.Fatalf("round %d: claim failed: %+v %v", round, grant, err)
		}
		f.expire(t, grant.AttemptID)
		ack := protocol.Ack{SchemaVersion: protocol.Version, AssignmentID: grant.AssignmentID,
			AttemptID: grant.AttemptID, AssignmentVersion: grant.AssignmentVersion,
			FencingToken: grant.FencingToken, CatalogSHA256: grant.CatalogSHA256,
			SourceSnapshotSHA256: grant.Source.SnapshotSHA256, HostFacts: f.facts}
		var ackErr, reapErr error
		var reaped int
		var start, done sync.WaitGroup
		start.Add(1)
		done.Add(2)
		go func() {
			defer done.Done()
			start.Wait()
			ackErr = f.store.AcknowledgeAgentAssignment(ctx, cert, ack)
		}()
		go func() {
			defer done.Done()
			start.Wait()
			reaped, reapErr = f.store.ReapAgentAssignments(ctx, f.catalog, 100)
		}()
		start.Done()
		done.Wait()
		if reapErr != nil {
			cancel()
			t.Fatalf("round %d: reaper failed: %v", round, reapErr)
		}
		state := f.attemptState(t, grant.AttemptID)
		switch state {
		case "running":
			if ackErr != nil {
				cancel()
				t.Fatalf("round %d: attempt is running but the ACK reported %v", round, ackErr)
			}
			if reaped != 0 {
				cancel()
				t.Fatalf("round %d: reaper claimed %d kills on an acknowledged attempt", round, reaped)
			}
		case "lost":
			if reaped != 1 {
				cancel()
				t.Fatalf("round %d: attempt is lost but the reaper reported %d kills", round, reaped)
			}
			if !errors.Is(ackErr, ErrConflict) {
				cancel()
				t.Fatalf("round %d: ACK after the reaper returned %v, want a conflict", round, ackErr)
			}
		default:
			cancel()
			t.Fatalf("round %d: attempt reached %q", round, state)
		}
		if err := CheckAttemptTransition("issued", state); err != nil {
			cancel()
			t.Fatalf("round %d: attempt took an edge the machine rejects: %v", round, err)
		}
		cancel()
	}
}

// TestIntegrationConcurrentCancellationAndClaimNeverBoth races cancellation
// against a claim for the same scheduled task. Cancellation terminalizes only
// unassigned work, so the task ends either cancelled with no attempt or
// running with exactly one; a cancelled task holding a live assignment would
// mean a fence was lost.
func TestIntegrationConcurrentCancellationAndClaimNeverBoth(t *testing.T) {
	cancelledRounds, claimedRounds := 0, 0
	for round := 0; round < 6; round++ {
		f := newConcurrencyFixture(t)
		cert := f.agent(t)
		run := f.run(t, 1)
		ctx, cancelCtx := context.WithTimeout(context.Background(), 60*time.Second)
		var grant *protocol.Assignment
		var claimErr, cancelErr error
		var start, done sync.WaitGroup
		start.Add(1)
		done.Add(2)
		go func() {
			defer done.Done()
			start.Wait()
			grant, claimErr = f.store.ClaimAgentTask(ctx, cert, f.facts, f.catalog, f.commit)
		}()
		go func() {
			defer done.Done()
			start.Wait()
			_, cancelErr = f.store.RequestRunCancellation(ctx, run.ID, "concurrency-operator")
		}()
		start.Done()
		done.Wait()
		if claimErr != nil {
			cancelCtx()
			t.Fatalf("round %d: claim failed: %v", round, claimErr)
		}
		if cancelErr != nil {
			cancelCtx()
			t.Fatalf("round %d: cancellation failed: %v", round, cancelErr)
		}
		rows := f.tasksOf(t, run.ID)
		if len(rows) != 1 {
			cancelCtx()
			t.Fatalf("round %d: run has %d tasks", round, len(rows))
		}
		for id, row := range rows {
			switch {
			case row.state == "cancelled" && row.attempts == 0:
				cancelledRounds++
				if grant != nil {
					cancelCtx()
					t.Fatalf("round %d: task %s was cancelled while assignment %s was issued",
						round, id, grant.AssignmentID)
				}
			case row.state == "running" && row.attempts == 1:
				claimedRounds++
				if grant == nil {
					cancelCtx()
					t.Fatalf("round %d: task %s is running with no assignment issued", round, id)
				}
			default:
				cancelCtx()
				t.Fatalf("round %d: task %s ended %q with %d attempts", round, id, row.state, row.attempts)
			}
		}
		cancelCtx()
	}
	if cancelledRounds+claimedRounds != 6 {
		t.Fatalf("accounted for %d of 6 rounds", cancelledRounds+claimedRounds)
	}
}

// TestIntegrationConcurrentReapersFenceEachAttemptOnce runs several reapers
// over the same expired assignments. They share one candidate query, so the
// per-attempt lock and the AttemptReapable recheck behind it are the only
// thing stopping a double kill: the kill counts have to sum to the number of
// expired attempts, not to reapers times attempts.
func TestIntegrationConcurrentReapersFenceEachAttemptOnce(t *testing.T) {
	const tasks, reapers = 3, 4
	f := newConcurrencyFixture(t)
	run := f.run(t, tasks)
	ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
	defer cancel()
	attempts := make([]string, 0, tasks)
	for i := 0; i < tasks; i++ {
		cert := f.agent(t)
		grant, err := f.store.ClaimAgentTask(ctx, cert, f.facts, f.catalog, f.commit)
		if err != nil || grant == nil {
			t.Fatalf("claim %d failed: %+v %v", i, grant, err)
		}
		f.expire(t, grant.AttemptID)
		attempts = append(attempts, grant.AttemptID)
	}
	counts := make([]int, reapers)
	failures := make([]error, reapers)
	var start, done sync.WaitGroup
	start.Add(1)
	for i := 0; i < reapers; i++ {
		done.Add(1)
		go func(i int) {
			defer done.Done()
			start.Wait()
			counts[i], failures[i] = f.store.ReapAgentAssignments(ctx, f.catalog, 100)
		}(i)
	}
	start.Done()
	done.Wait()
	total := 0
	for i, err := range failures {
		if err != nil {
			t.Fatalf("reaper %d failed: %v", i, err)
		}
		total += counts[i]
	}
	if total != tasks {
		t.Fatalf("%d reapers killed %d attempts, want %d", reapers, total, tasks)
	}
	for _, attemptID := range attempts {
		if state := f.attemptState(t, attemptID); state != "lost" {
			t.Fatalf("expired attempt %s is %q", attemptID, state)
		}
	}
	for id, row := range f.tasksOf(t, run.ID) {
		if row.state != "scheduled" && row.state != "lost" {
			t.Fatalf("reaped task %s is %q", id, row.state)
		}
		if err := CheckTaskTransition("running", row.state); err != nil {
			t.Fatalf("reaped task %s took an edge the machine rejects: %v", id, err)
		}
		if row.attempts != 1 {
			t.Fatalf("reaped task %s carries %d attempts", id, row.attempts)
		}
	}
}
