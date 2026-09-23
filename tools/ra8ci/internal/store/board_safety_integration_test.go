//go:build integration

package store

import (
	"context"
	"errors"
	"os"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
	"github.com/jackc/pgx/v5/pgxpool"
)

func activeTestBoard(t *testing.T, ctx context.Context) (*Store, *pgxpool.Pool, BoardActor, BoardActor, board.Snapshot, board.Waiter, time.Time) {
	t.Helper()
	s, pool := integrationStore(t)
	id := "board-" + mustID(t)
	human := boardTestActor(t, ctx, s, pool, id, "human", "board_human")
	agent := boardTestActor(t, ctx, s, pool, id, "board_agent", "board_agent")
	now := time.Now().UTC().Truncate(time.Second)
	w := board.Waiter{ID: mustID(t), LeaseID: mustID(t), Holder: human.ID(), Class: board.ClassHuman,
		Reason: "proof safety", Duration: time.Minute}
	granted, _, err := s.ApplyBoardCommand(ctx, human, board.Enqueue{Waiter: w}, 0, nil, nil, now)
	if err != nil {
		t.Fatal(err)
	}
	active, _, err := s.ApplyBoardCommand(ctx, agent, board.AcknowledgeGrant{
		LeaseID: w.LeaseID, Generation: granted.Generation, InstalledGeneration: granted.Generation,
	}, granted.Version, nil, nil, now.Add(time.Second))
	if err != nil || active.Phase != board.Active {
		t.Fatalf("activate: %+v %v", active, err)
	}
	return s, pool, human, agent, active, w, now
}

func TestIntegrationNeutralChallengeRejectsStaleVersionAndReplay(t *testing.T) {
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	s, pool, human, _, active, w, now := activeTestBoard(t, ctx)
	c, err := s.IssueBoardNeutralChallenge(ctx, human, active.Version, "release")
	if err != nil {
		t.Fatal(err)
	}
	second := board.Waiter{ID: mustID(t), LeaseID: mustID(t), Holder: human.ID(), Class: board.ClassHuman,
		Reason: "queue behind holder", Duration: time.Minute}
	queued, _, err := s.ApplyBoardCommand(ctx, human, board.Enqueue{Waiter: second}, active.Version, nil, nil, now.Add(2*time.Second))
	if err != nil || queued.Version != active.Version+1 {
		t.Fatalf("queue: %+v %v", queued, err)
	}
	receipt := []byte("signed-neutral-test")
	failed, _, err := s.ApplyBoardCommand(ctx, human, board.Release{
		LeaseID: w.LeaseID, Generation: active.Generation,
	}, queued.Version, &NeutralSubmission{ChallengeID: c.ID, Receipt: receipt},
		exactNeutralVerifier{challenge: c, receipt: string(receipt)}, now.Add(3*time.Second))
	if !board.IsCode(err, board.RecoveryNecessary) || failed.Phase != board.RecoveryRequired {
		t.Fatalf("stale proof released board: %+v %v", failed, err)
	}
	var outcome string
	if err := pool.QueryRow(ctx, "SELECT outcome FROM board_neutral_challenges WHERE id=$1", c.ID).Scan(&outcome); err != nil || outcome != "rejected" {
		t.Fatalf("stale challenge was not consumed: %q %v", outcome, err)
	}
	// A consumed challenge must not be usable again, even by a correct verifier.
	tx, err := s.pool.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer func() { _ = tx.Rollback(ctx) }()
	if proof, err := consumeNeutral(ctx, tx, failed, "release", &NeutralSubmission{ChallengeID: c.ID, Receipt: receipt},
		exactNeutralVerifier{challenge: c, receipt: string(receipt)}); !errors.Is(err, ErrDenied) || proof != "" {
		t.Fatalf("consumed proof replayed: %q %v", proof, err)
	}
}

func TestIntegrationNeutralChallengeRejectsProfileAndHighWaterChanges(t *testing.T) {
	for _, change := range []string{"profile", "high_water"} {
		t.Run(change, func(t *testing.T) {
			ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
			defer cancel()
			s, pool, human, _, active, w, now := activeTestBoard(t, ctx)
			c, err := s.IssueBoardNeutralChallenge(ctx, human, active.Version, "release")
			if err != nil {
				t.Fatal(err)
			}
			if change == "profile" {
				_, err = pool.Exec(ctx, "UPDATE board_fixture_profiles SET profile_sha256=$2,version=version+1 WHERE board_id=$1",
					active.BoardID, strings.Repeat("b", 64))
			} else {
				_, err = pool.Exec(ctx, "UPDATE board_neutral_challenges SET agent_high_water=agent_high_water+1 WHERE id=$1", c.ID)
			}
			if err != nil {
				t.Fatal(err)
			}
			failed, _, err := s.ApplyBoardCommand(ctx, human, board.Release{
				LeaseID: w.LeaseID, Generation: active.Generation,
			}, active.Version, &NeutralSubmission{ChallengeID: c.ID, Receipt: []byte("signed-neutral-test")},
				exactNeutralVerifier{challenge: c, receipt: "signed-neutral-test"}, now.Add(2*time.Second))
			if !board.IsCode(err, board.RecoveryNecessary) || failed.Phase != board.RecoveryRequired {
				t.Fatalf("altered %s proof released board: %+v %v", change, failed, err)
			}
		})
	}
}

func TestIntegrationNeutralRecoveryBindsPlanAndClosesSession(t *testing.T) {
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	s, pool, human, _, active, w, now := activeTestBoard(t, ctx)
	operator := boardTestActor(t, ctx, s, pool, active.BoardID, "human", "operator")
	required, _, err := s.ApplyBoardCommand(ctx, human, board.Release{
		LeaseID: w.LeaseID, Generation: active.Generation,
	}, active.Version, nil, nil, now.Add(2*time.Second))
	if !board.IsCode(err, board.RecoveryNecessary) || required.Phase != board.RecoveryRequired {
		t.Fatalf("enter recovery: %+v %v", required, err)
	}
	planID := "restore-" + mustID(t)
	recovering, _, err := s.ApplyBoardCommand(ctx, operator, board.BeginRecovery{
		PlanID: planID, Reason: "restore approved image",
	}, required.Version, nil, nil, now.Add(3*time.Second))
	if err != nil || recovering.Phase != board.Recovering {
		t.Fatalf("begin recovery: %+v %v", recovering, err)
	}
	c, err := s.IssueBoardNeutralChallenge(ctx, operator, recovering.Version, "recovery")
	if err != nil || c.RecoveryPlanID != planID || c.LeaseID != w.LeaseID {
		t.Fatalf("recovery challenge context: %+v %v", c, err)
	}
	receipt := "signed-recovered-fixture"
	ready, _, err := s.ApplyBoardCommand(ctx, operator, board.CompleteRecovery{
		AgentHighWater: recovering.Generation + 100, // untrusted request-body claim
	}, recovering.Version, &NeutralSubmission{ChallengeID: c.ID, Receipt: []byte(receipt)},
		exactNeutralVerifier{challenge: c, receipt: receipt}, now.Add(4*time.Second))
	if err != nil || ready.Phase != board.Ready || ready.Lease != nil ||
		ready.Generation != recovering.Generation || ready.AgentHighWater != recovering.AgentHighWater {
		t.Fatalf("complete recovery: %+v %v", ready, err)
	}
	var activeSessions, activePlans int
	if err := pool.QueryRow(ctx, "SELECT COUNT(*) FROM board_sessions WHERE board_id=$1 AND ended_at IS NULL", active.BoardID).Scan(&activeSessions); err != nil {
		t.Fatal(err)
	}
	if err := pool.QueryRow(ctx, "SELECT COUNT(*) FROM board_recovery_context WHERE board_id=$1 AND ended_at IS NULL", active.BoardID).Scan(&activePlans); err != nil {
		t.Fatal(err)
	}
	if activeSessions != 0 || activePlans != 0 {
		t.Fatalf("recovery left live context: sessions=%d plans=%d", activeSessions, activePlans)
	}
}

func TestIntegrationCrossBoardProjectionIDsCannotOverwrite(t *testing.T) {
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	s, pool := integrationStore(t)
	firstID, secondID := "board-"+mustID(t), "board-"+mustID(t)
	first := boardTestActor(t, ctx, s, pool, firstID, "human", "board_human")
	second := boardTestActor(t, ctx, s, pool, secondID, "human", "board_human")
	now := time.Now().UTC().Truncate(time.Second)
	w := board.Waiter{ID: mustID(t), LeaseID: mustID(t), Holder: first.ID(), Class: board.ClassHuman,
		Reason: "first board", Duration: time.Minute}
	if _, _, err := s.ApplyBoardCommand(ctx, first, board.Enqueue{Waiter: w}, 0, nil, nil, now); err != nil {
		t.Fatal(err)
	}
	for _, collide := range []string{"waiter", "lease"} {
		candidate := board.Waiter{ID: mustID(t), LeaseID: mustID(t), Holder: second.ID(), Class: board.ClassHuman,
			Reason: "collision", Duration: time.Minute}
		if collide == "waiter" {
			candidate.ID = w.ID
		} else {
			candidate.LeaseID = w.LeaseID
		}
		_, _, err := s.ApplyBoardCommand(ctx, second, board.Enqueue{Waiter: candidate}, 0, nil, nil, now.Add(time.Second))
		if !errors.Is(err, ErrConflict) {
			t.Fatalf("%s collision did not abort transition: %v", collide, err)
		}
		var count int
		if err := pool.QueryRow(ctx, "SELECT COUNT(*) FROM board_leases WHERE id=$1 AND board_id=$2", w.LeaseID, firstID).Scan(&count); err != nil || count != 1 {
			t.Fatalf("first lease overwritten by %s: %d %v", collide, count, err)
		}
		if err := pool.QueryRow(ctx, "SELECT COUNT(*) FROM board_waiters WHERE id=$1 AND board_id=$2", w.ID, firstID).Scan(&count); err != nil || count != 1 {
			t.Fatalf("first waiter overwritten by %s: %d %v", collide, count, err)
		}
	}
}

func TestIntegrationRuntimeCannotMutateAppendOnlyHistory(t *testing.T) {
	s, _ := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	for _, relation := range []string{"audit", "board_events", "run_events"} {
		var update, remove, truncate bool
		if err := s.pool.QueryRow(ctx, `SELECT has_table_privilege(current_user,$1,'UPDATE'),
			has_table_privilege(current_user,$1,'DELETE'),has_table_privilege(current_user,$1,'TRUNCATE')`, relation).
			Scan(&update, &remove, &truncate); err != nil || update || remove || truncate {
			t.Fatalf("runtime role can mutate %s: %v %v %v, %v", relation, update, remove, truncate, err)
		}
	}
	if err := s.CheckSchema(ctx); err != nil {
		t.Fatal(err)
	}
	if owner, err := Open(ctx, os.Getenv("RA8CI_TEST_PG_DSN")); err == nil {
		owner.Close()
		t.Fatal("table owner was accepted as runtime role")
	} else if !errors.Is(err, ErrUnavailable) {
		t.Fatalf("unexpected owner-role rejection: %v", err)
	}
}
