//go:build integration

package store

import (
	"context"
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/sha256"
	"crypto/tls"
	"crypto/x509"
	"encoding/hex"
	"errors"
	"math/big"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
	"github.com/jackc/pgx/v5/pgxpool"
)

const boardTestRepo = "bsikar/ra8-firmware"

func TestIntegrationBoardExpiryDenialCommitsAtomically(t *testing.T) {
	s, pool := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	boardID := "board-" + mustID(t)
	human := boardTestActor(t, ctx, s, pool, boardID, "human", "board_human")
	now := time.Now().UTC().Truncate(time.Second)
	waiter := board.Waiter{ID: mustID(t), LeaseID: mustID(t), Holder: human.ID(), Class: board.ClassHuman, Reason: "integration expiry", Duration: time.Second}
	granted, events, err := s.ApplyBoardCommand(ctx, human, board.Enqueue{Actor: "forged", Waiter: waiter}, 0, nil, nil, now)
	if err != nil || granted.Phase != board.GrantPending || granted.Version != 1 || len(events) != 2 {
		t.Fatalf("initial grant: snapshot=%+v events=%+v err=%v", granted, events, err)
	}
	if events[0].Actor != human.ID() || events[1].Actor != human.ID() {
		t.Fatal("request-body actor was not replaced with authenticated identity")
	}
	var projectedWaiters, projectedLeases int
	if err := pool.QueryRow(ctx, "SELECT COUNT(*) FROM board_waiters WHERE board_id=$1 AND state='granted'", boardID).Scan(&projectedWaiters); err != nil {
		t.Fatal(err)
	}
	if err := pool.QueryRow(ctx, "SELECT COUNT(*) FROM board_leases WHERE board_id=$1 AND state='pending'", boardID).Scan(&projectedLeases); err != nil {
		t.Fatal(err)
	}
	if projectedWaiters != 1 || projectedLeases != 1 {
		t.Fatalf("typed projection missing waiter/lease: %d/%d", projectedWaiters, projectedLeases)
	}
	expired, deniedEvents, err := s.ApplyBoardCommand(ctx, human, board.Extend{
		Actor: "forged", LeaseID: waiter.LeaseID, Generation: granted.Generation,
		NewExpiry: now.Add(time.Minute), Reason: "too late",
	}, granted.Version, nil, nil, now.Add(2*time.Second))
	if !board.IsCode(err, board.Expired) || expired.Phase != board.RecoveryRequired || expired.Version != 2 || len(deniedEvents) != 2 || deniedEvents[0].Kind != board.LeaseExpired || deniedEvents[1].Kind != board.ActionDenied {
		t.Fatalf("expiry and rejection not returned together: snapshot=%+v events=%+v err=%v", expired, deniedEvents, err)
	}
	stored, err := s.GetBoard(ctx, boardID)
	if err != nil || stored.Phase != board.RecoveryRequired || stored.Version != 2 {
		t.Fatalf("reducer error rolled back expiry: %+v %v", stored, err)
	}
	var eventCount, auditCount, liveLeaseCount int
	if err := pool.QueryRow(ctx, "SELECT COUNT(*) FROM board_events WHERE board_id=$1", boardID).Scan(&eventCount); err != nil {
		t.Fatal(err)
	}
	if err := pool.QueryRow(ctx, "SELECT COUNT(*) FROM audit WHERE target_type='board' AND target_id=$1", boardID).Scan(&auditCount); err != nil {
		t.Fatal(err)
	}
	if err := pool.QueryRow(ctx, "SELECT COUNT(*) FROM board_leases WHERE board_id=$1 AND state IN ('pending','active')", boardID).Scan(&liveLeaseCount); err != nil {
		t.Fatal(err)
	}
	if eventCount != 4 || auditCount != 4 || liveLeaseCount != 0 {
		t.Fatalf("atomic projection/audit mismatch: events=%d audit=%d live=%d", eventCount, auditCount, liveLeaseCount)
	}
	if _, err := pool.Exec(ctx, "UPDATE board_events SET reason='tampered' WHERE board_id=$1", boardID); err == nil {
		t.Fatal("board event history was mutable")
	}
	_, _, err = s.TickBoard(ctx, boardID, granted.Version, now.Add(3*time.Second))
	if !errors.Is(err, ErrConflict) {
		t.Fatalf("stale version was not rejected: %v", err)
	}
}

func TestIntegrationVerifiedNeutralReleaseAndActorScope(t *testing.T) {
	s, pool := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	boardID := "board-" + mustID(t)
	human := boardTestActor(t, ctx, s, pool, boardID, "human", "board_human")
	agent := boardTestActor(t, ctx, s, pool, boardID, "board_agent", "board_agent")
	now := time.Now().UTC().Truncate(time.Second)
	waiter := board.Waiter{ID: mustID(t), LeaseID: mustID(t), Holder: human.ID(), Class: board.ClassHuman, Reason: "verified release", Duration: time.Minute}
	granted, _, err := s.ApplyBoardCommand(ctx, human, board.Enqueue{Waiter: waiter}, 0, nil, nil, now)
	if err != nil {
		t.Fatal(err)
	}
	active, _, err := s.ApplyBoardCommand(ctx, agent, board.AcknowledgeGrant{
		LeaseID: waiter.LeaseID, Generation: granted.Generation, InstalledGeneration: granted.Generation,
	}, granted.Version, nil, nil, now.Add(time.Second))
	if err != nil || active.Phase != board.Active {
		t.Fatalf("agent acknowledgement failed: %+v %v", active, err)
	}
	wrongBoard := boardTestActor(t, ctx, s, pool, "board-"+mustID(t), "human", "board_human")
	if _, _, err := s.ApplyBoardCommand(ctx, wrongBoard, board.Release{LeaseID: waiter.LeaseID, Generation: active.Generation, NeutralReceipt: "forged"}, 0, nil, nil, now.Add(2*time.Second)); !errors.Is(err, ErrDenied) {
		t.Fatalf("cross-board actor accepted: %v", err)
	}
	receipt := "private-test-receipt-" + mustID(t)
	challenge, err := s.IssueBoardNeutralChallenge(ctx, human, active.Version, "release")
	if err != nil {
		t.Fatal(err)
	}
	verifier := exactNeutralVerifier{challenge: challenge, receipt: receipt}
	released, events, err := s.ApplyBoardCommand(ctx, human, board.Release{
		Actor: "forged", LeaseID: waiter.LeaseID, Generation: active.Generation, NeutralReceipt: "forged-unverified",
	}, active.Version, &NeutralSubmission{ChallengeID: challenge.ID, Receipt: []byte(receipt)}, verifier, now.Add(2*time.Second))
	if err != nil || released.Phase != board.Ready || released.Lease != nil || len(events) != 1 || events[0].Kind != board.LeaseReleased {
		t.Fatalf("verified release failed: %+v %+v %v", released, events, err)
	}
	if strings.Contains(events[0].Reason, receipt) || !strings.HasPrefix(events[0].Reason, "sha256:") {
		t.Fatalf("raw neutral receipt leaked to audit event: %q", events[0].Reason)
	}
	var endReason string
	if err := pool.QueryRow(ctx, "SELECT end_reason FROM board_leases WHERE id=$1", waiter.LeaseID).Scan(&endReason); err != nil || endReason != "released" {
		t.Fatalf("typed lease was not ended: %q %v", endReason, err)
	}
}

func TestIntegrationMissingNeutralProofRequiresRecovery(t *testing.T) {
	s, pool := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	boardID := "board-" + mustID(t)
	human := boardTestActor(t, ctx, s, pool, boardID, "human", "board_human")
	agent := boardTestActor(t, ctx, s, pool, boardID, "board_agent", "board_agent")
	now := time.Now().UTC().Truncate(time.Second)
	waiter := board.Waiter{ID: mustID(t), LeaseID: mustID(t), Holder: human.ID(), Class: board.ClassHuman, Reason: "missing proof", Duration: time.Minute}
	granted, _, err := s.ApplyBoardCommand(ctx, human, board.Enqueue{Waiter: waiter}, 0, nil, nil, now)
	if err != nil {
		t.Fatal(err)
	}
	active, _, err := s.ApplyBoardCommand(ctx, agent, board.AcknowledgeGrant{LeaseID: waiter.LeaseID, Generation: granted.Generation, InstalledGeneration: granted.Generation}, granted.Version, nil, nil, now.Add(time.Second))
	if err != nil {
		t.Fatal(err)
	}
	recovery, events, err := s.ApplyBoardCommand(ctx, human, board.Release{LeaseID: waiter.LeaseID, Generation: active.Generation, NeutralReceipt: "forged"}, active.Version, nil, nil, now.Add(2*time.Second))
	if !board.IsCode(err, board.RecoveryNecessary) || recovery.Phase != board.RecoveryRequired || len(events) != 1 || events[0].Kind != board.RecoveryNeeded {
		t.Fatalf("unverified release escaped recovery: %+v %+v %v", recovery, events, err)
	}
}

func TestIntegrationConcurrentBoardCAS(t *testing.T) {
	s, pool := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	boardID := "board-" + mustID(t)
	human := boardTestActor(t, ctx, s, pool, boardID, "human", "board_human")
	now := time.Now().UTC().Truncate(time.Second)
	initial, events, err := s.TickBoard(ctx, boardID, 0, now)
	if err != nil || initial.Version != 0 || len(events) != 0 {
		t.Fatalf("board initialization: %+v %+v %v", initial, events, err)
	}
	var wg sync.WaitGroup
	results := make(chan error, 2)
	for i := 0; i < 2; i++ {
		waiter := board.Waiter{ID: mustID(t), LeaseID: mustID(t), Holder: human.ID(), Class: board.ClassHuman, Reason: "racing request", Duration: time.Minute}
		wg.Add(1)
		go func() {
			defer wg.Done()
			_, _, err := s.ApplyBoardCommand(ctx, human, board.Enqueue{Waiter: waiter}, 0, nil, nil, now.Add(time.Second))
			results <- err
		}()
	}
	wg.Wait()
	close(results)
	accepted, conflicted := 0, 0
	for err := range results {
		if err == nil {
			accepted++
		} else if errors.Is(err, ErrConflict) {
			conflicted++
		} else {
			t.Fatalf("unexpected concurrent transition error: %v", err)
		}
	}
	if accepted != 1 || conflicted != 1 {
		t.Fatalf("CAS accepted=%d conflicted=%d, want one each", accepted, conflicted)
	}
	var live int
	if err := pool.QueryRow(ctx, "SELECT COUNT(*) FROM board_leases WHERE board_id=$1 AND state IN ('pending','active')", boardID).Scan(&live); err != nil || live != 1 {
		t.Fatalf("board exclusivity lost: live=%d err=%v", live, err)
	}
}

func TestIntegrationRevokedBoardActorCannotMutate(t *testing.T) {
	s, pool := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	boardID := "board-" + mustID(t)
	human := boardTestActor(t, ctx, s, pool, boardID, "human", "board_human")
	if _, err := pool.Exec(ctx, "UPDATE api_principals SET revoked_at=clock_timestamp() WHERE principal_id=$1", human.ID()); err != nil {
		t.Fatal(err)
	}
	waiter := board.Waiter{ID: mustID(t), LeaseID: mustID(t), Holder: human.ID(), Class: board.ClassHuman, Reason: "revoked actor", Duration: time.Minute}
	_, _, err := s.ApplyBoardCommand(ctx, human, board.Enqueue{Waiter: waiter}, 0, nil, nil, time.Now().UTC())
	if !errors.Is(err, ErrDenied) {
		t.Fatalf("revoked board actor mutated state: %v", err)
	}
	if _, err := s.GetBoard(ctx, boardID); !errors.Is(err, ErrNotFound) {
		t.Fatalf("revoked actor created a board: %v", err)
	}
}

type exactNeutralVerifier struct {
	challenge NeutralChallenge
	receipt   string
}

func (v exactNeutralVerifier) VerifyNeutralReceipt(_ context.Context, challenge NeutralChallenge, receipt []byte) error {
	if challenge != v.challenge || string(receipt) != v.receipt {
		return ErrDenied
	}
	return nil
}

func boardTestActor(t *testing.T, ctx context.Context, s *Store, pool *pgxpool.Pool, boardID, kind, role string) BoardActor {
	t.Helper()
	if pool == nil {
		pool = s.pool
	}
	if pool != s.pool {
		_, err := pool.Exec(ctx, `INSERT INTO board_fixture_profiles
			(board_id,fixture_revision,profile_sha256,restore_policy)
			VALUES ($1,'fixture-v1',$2,'restore-image') ON CONFLICT (board_id) DO NOTHING`,
			boardID, strings.Repeat("a", 64))
		if err != nil {
			t.Fatal(err)
		}
	}
	key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	certTemplate := &x509.Certificate{
		SerialNumber: big.NewInt(time.Now().UnixNano()), NotBefore: time.Now().Add(-time.Minute),
		NotAfter: time.Now().Add(time.Hour), KeyUsage: x509.KeyUsageDigitalSignature,
		ExtKeyUsage: []x509.ExtKeyUsage{x509.ExtKeyUsageClientAuth},
	}
	der, err := x509.CreateCertificate(rand.Reader, certTemplate, certTemplate, &key.PublicKey, key)
	if err != nil {
		t.Fatal(err)
	}
	cert, err := x509.ParseCertificate(der)
	if err != nil {
		t.Fatal(err)
	}
	id := mustID(t)
	sum := sha256.Sum256(cert.Raw)
	_, err = pool.Exec(ctx, `INSERT INTO api_principals
		(cert_sha256, principal_id, kind, expires_at) VALUES ($1,$2,$3,clock_timestamp()+interval '1 hour')`,
		hex.EncodeToString(sum[:]), id, kind)
	if err != nil {
		t.Fatal(err)
	}
	_, err = pool.Exec(ctx, `INSERT INTO api_grants (principal_id,repository,role,board_id)
		VALUES ($1,$2,$3,$4)`, id, boardTestRepo, role, boardID)
	if err != nil {
		t.Fatal(err)
	}
	actor, err := s.AuthorizeBoardPeer(ctx, &tls.ConnectionState{
		PeerCertificates: []*x509.Certificate{cert}, VerifiedChains: [][]*x509.Certificate{{cert}},
	}, boardTestRepo, boardID)
	if err != nil {
		t.Fatal(err)
	}
	return actor
}
func TestIntegrationBoardSegmentAndHumanWaiterSerialize(t *testing.T) {
	s, pool := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	boardID := "board-" + mustID(t)
	agent := boardTestActor(t, ctx, s, pool, boardID, "agent", "submitter")
	human := boardTestActor(t, ctx, s, pool, boardID, "human", "board_human")
	boardAgent := boardTestActor(t, ctx, s, pool, boardID, "board_agent", "board_agent")
	now := time.Now().UTC()
	waiter := board.Waiter{ID: mustID(t), LeaseID: mustID(t), Holder: agent.ID(),
		Class: board.ClassAI, Reason: "bounded board segment", Duration: time.Minute}
	pending, _, err := s.ApplyBoardCommand(ctx, agent, board.Enqueue{Waiter: waiter}, 0, nil, nil, now)
	if err != nil {
		t.Fatal(err)
	}
	active, _, err := s.ApplyBoardCommand(ctx, boardAgent, board.AcknowledgeGrant{
		LeaseID: waiter.LeaseID, Generation: pending.Generation, InstalledGeneration: pending.Generation,
	}, pending.Version, nil, nil, now.Add(time.Second))
	if err != nil || active.Phase != board.Active {
		t.Fatalf("agent did not install lease: phase=%s err=%v", active.Phase, err)
	}
	args := []byte(`{"argv":[],"hil":{"board_id":"` + boardID + `","board_model":"EK-RA8D2","manifest_path":"examples/ek_ra8d2/hw_validated/hil/demo/hil.conf","program_family":"uart-demo","mode":"uart_scrape","observation_step":"observe","flash_restore_seconds":10}}`)
	run, err := s.CreateRun(ctx, CreateRunInput{Trigger: "integration", ActorID: agent.ID(), Repository: boardTestRepo,
		CommitSHA: strings.Repeat("a", 40), SnapshotSHA256: strings.Repeat("b", 64), CatalogSHA256: strings.Repeat("c", 64),
		Tasks: []TaskInput{
			{Key: "bounded-segment", Name: "hil-run", Arguments: args, Tier: "required", Scope: "hil", HostClass: "hil-lab", DeadlineSeconds: 30},
			{Key: "second-hil", Name: "hil-run", Arguments: args, Tier: "required", Scope: "hil", HostClass: "hil-lab", DeadlineSeconds: 30},
		}})
	if err != nil {
		t.Fatal(err)
	}
	start := testStart(run.Tasks[0].ID)
	attempt, err := s.StartBoardHILAttempt(ctx, boardAgent, run.Tasks[0].ID, waiter.LeaseID, start)
	if err != nil {
		t.Fatal(err)
	}
	replayedAttempt, replayErr := s.StartBoardHILAttempt(ctx, boardAgent, run.Tasks[0].ID, waiter.LeaseID, start)
	if replayErr != nil || replayedAttempt.ID != attempt.ID {
		t.Fatalf("HIL claim retry did not return the original attempt: original=%s replay=%+v err=%v", attempt.ID, replayedAttempt, replayErr)
	}
	if _, secondErr := s.StartBoardHILAttempt(ctx, boardAgent, run.Tasks[1].ID, waiter.LeaseID, start); !errors.Is(secondErr, ErrConflict) {
		t.Fatalf("one board lease started concurrent HIL tasks: %v", secondErr)
	}
	token := board.Token{BoardID: boardID, LeaseID: waiter.LeaseID, Generation: active.Generation}
	segment, err := s.BeginBoardSegment(ctx, boardAgent, active.Version, token, attempt.ID, "flash", 20*time.Second, 3*time.Second)
	if err != nil || segment.ID == "" || !segment.DeadlineAt.After(segment.StartedAt) {
		t.Fatalf("bounded segment did not start: segment=%+v err=%v", segment, err)
	}
	humanWaiter := board.Waiter{ID: mustID(t), LeaseID: mustID(t), Holder: human.ID(),
		Class: board.ClassHuman, Reason: "human board use", Duration: time.Minute}
	yielding, _, err := s.ApplyBoardCommand(ctx, human, board.Enqueue{Waiter: humanWaiter}, active.Version, nil, nil, now.Add(2*time.Second))
	if err != nil || yielding.Phase != board.YieldRequested {
		t.Fatalf("human waiter did not request cooperative yield: phase=%s err=%v", yielding.Phase, err)
	}
	if _, err := s.BeginBoardSegment(ctx, agent, yielding.Version, token, attempt.ID, "next", time.Second, 0); err == nil {
		t.Fatal("new segment started after human waiter queued")
	}
	if err := s.FinishBoardSegment(ctx, boardAgent, segment.ID, token, attempt.ID, "yielded"); err != nil {
		t.Fatalf("holder could not finish its already-started bounded segment: %v", err)
	}
	var outcome string
	var endedAt time.Time
	if err := pool.QueryRow(ctx, "SELECT outcome,ended_at FROM board_segments WHERE id=$1", segment.ID).Scan(&outcome, &endedAt); err != nil ||
		outcome != "yielded" || endedAt.IsZero() {
		t.Fatalf("segment completion not durable: outcome=%q ended=%v err=%v", outcome, endedAt, err)
	}
}
