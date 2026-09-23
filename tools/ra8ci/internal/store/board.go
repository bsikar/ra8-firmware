package store

import (
	"context"
	"crypto/rand"
	"crypto/sha256"
	"crypto/tls"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"math"
	"strings"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgconn"
)

// BoardActor is issued only after a verified mTLS peer is mapped to a board
// grant. Its fields are private so an HTTP body cannot manufacture identity.
type BoardActor struct {
	id          string
	kind        string
	role        string
	boardID     string
	repository  string
	certificate string
}

func (a BoardActor) ID() string { return a.id }

// AuthorizeBoardPeer binds a verified TLS leaf to an active board-scoped grant.
// Operator grants may be repository-wide; all other grants name the board.
func (s *Store) AuthorizeBoardPeer(ctx context.Context, peer *tls.ConnectionState, repository, boardID string) (BoardActor, error) {
	if s == nil || s.pool == nil || peer == nil || len(peer.PeerCertificates) == 0 || len(peer.VerifiedChains) == 0 || len(peer.VerifiedChains[0]) == 0 || !peer.PeerCertificates[0].Equal(peer.VerifiedChains[0][0]) || repository == "" || !validBoardID(boardID) {
		return BoardActor{}, ErrDenied
	}
	sum := sha256.Sum256(peer.PeerCertificates[0].Raw)
	var actor BoardActor
	err := s.pool.QueryRow(ctx, `SELECT p.principal_id, p.kind, g.role FROM api_principals p
		JOIN api_grants g ON g.principal_id=p.principal_id
		WHERE p.cert_sha256=$1 AND p.revoked_at IS NULL AND p.expires_at>clock_timestamp()
		AND g.repository=$2 AND (g.board_id=$3 OR (g.board_id='' AND g.role='operator'))
		AND g.role IN ('board_human','board_agent','submitter','operator')
		AND ((p.kind='human' AND g.role IN ('board_human','operator'))
			OR (p.kind='board_agent' AND g.role='board_agent')
			OR (p.kind IN ('github_app','agent') AND g.role='submitter'))
		ORDER BY CASE g.role WHEN 'operator' THEN 0 WHEN 'board_agent' THEN 1
		WHEN 'board_human' THEN 2 ELSE 3 END LIMIT 1`,
		hex.EncodeToString(sum[:]), repository, boardID).Scan(&actor.id, &actor.kind, &actor.role)
	if errors.Is(err, pgx.ErrNoRows) {
		return BoardActor{}, ErrDenied
	}
	if err != nil {
		return BoardActor{}, fmt.Errorf("%w: board authorization: %v", ErrUnavailable, err)
	}
	actor.boardID = boardID
	actor.repository = repository
	actor.certificate = hex.EncodeToString(sum[:])
	return actor, nil
}

// NeutralChallenge freezes the state and approved fixture context that the
// board agent must attest before a release or recovery can become authoritative.
type NeutralChallenge struct {
	ID              string    `json:"id"`
	Nonce           string    `json:"nonce"`
	BoardID         string    `json:"board_id"`
	Purpose         string    `json:"purpose"`
	LeaseID         string    `json:"lease_id,omitempty"`
	Generation      uint64    `json:"generation"`
	SnapshotVersion uint64    `json:"snapshot_version"`
	AgentHighWater  uint64    `json:"agent_high_water"`
	FixtureRevision string    `json:"fixture_revision"`
	ProfileSHA256   string    `json:"profile_sha256"`
	RestorePolicy   string    `json:"restore_policy"`
	RecoveryPlanID  string    `json:"recovery_plan_id,omitempty"`
	IssuedAt        time.Time `json:"issued_at"`
	ExpiresAt       time.Time `json:"expires_at"`
}

// NeutralSubmission contains untrusted receipt bytes and a persisted one-use
// challenge ID. The store verifies and consumes both in the transition tx.
type NeutralSubmission struct {
	ChallengeID string
	Receipt     []byte
}

// NeutralReceiptVerifier authenticates the board agent and proves neutral
// hardware state for every field of the exact database-issued challenge.
type NeutralReceiptVerifier interface {
	VerifyNeutralReceipt(context.Context, NeutralChallenge, []byte) error
}

// IssueBoardNeutralChallenge derives a one-use challenge from the locked
// snapshot and its pinned fixture/recovery context. Missing context fails shut.
func (s *Store) IssueBoardNeutralChallenge(ctx context.Context, actor BoardActor, expectedVersion uint64, purpose string) (NeutralChallenge, error) {
	if s == nil || s.pool == nil || actor.id == "" || actor.kind == "system" || !validBoardID(actor.boardID) || expectedVersion > math.MaxInt64 || (purpose != "release" && purpose != "recovery") {
		return NeutralChallenge{}, fmt.Errorf("%w: neutral challenge request", ErrInvalid)
	}
	tx, err := s.pool.BeginTx(ctx, pgx.TxOptions{IsoLevel: pgx.Serializable})
	if err != nil {
		return NeutralChallenge{}, fmt.Errorf("%w: begin challenge: %v", ErrUnavailable, err)
	}
	defer func() { _ = tx.Rollback(ctx) }()
	if _, err := tx.Exec(ctx, "SELECT pg_advisory_xact_lock(hashtextextended($1, 1))", actor.boardID); err != nil {
		return NeutralChallenge{}, fmt.Errorf("%w: board lock: %v", ErrUnavailable, err)
	}
	if err := revalidateBoardActor(ctx, tx, actor); err != nil {
		return NeutralChallenge{}, err
	}
	snapshot, err := loadOrCreateBoard(ctx, tx, actor.boardID)
	if err != nil {
		return NeutralChallenge{}, err
	}
	if snapshot.Version != expectedVersion {
		return NeutralChallenge{}, fmt.Errorf("%w: stale board version", ErrConflict)
	}
	if purpose == "release" {
		if !ownsLease(snapshot, actor.id) || !liveBoardPhase(snapshot.Phase) {
			return NeutralChallenge{}, ErrDenied
		}
	} else if actor.role != "operator" || snapshot.Phase != board.Recovering {
		return NeutralChallenge{}, ErrDenied
	}
	challenge, err := neutralContext(ctx, tx, snapshot, purpose)
	if err != nil {
		return NeutralChallenge{}, err
	}
	var nonce [32]byte
	if _, err := rand.Read(nonce[:]); err != nil {
		return NeutralChallenge{}, fmt.Errorf("%w: challenge entropy: %v", ErrUnavailable, err)
	}
	challenge.Nonce = hex.EncodeToString(nonce[:])
	challenge.ID, err = NewID()
	if err != nil {
		return NeutralChallenge{}, fmt.Errorf("%w: challenge ID: %v", ErrUnavailable, err)
	}
	if err := tx.QueryRow(ctx, "SELECT clock_timestamp()").Scan(&challenge.IssuedAt); err != nil {
		return NeutralChallenge{}, fmt.Errorf("%w: challenge clock: %v", ErrUnavailable, err)
	}
	if purpose == "release" && !challenge.IssuedAt.Before(snapshot.Lease.ExpiresAt) {
		return NeutralChallenge{}, fmt.Errorf("%w: lease expired before challenge", ErrConflict)
	}
	challenge.ExpiresAt = challenge.IssuedAt.Add(30 * time.Second)
	_, err = tx.Exec(ctx, `INSERT INTO board_neutral_challenges
		(id,nonce,board_id,purpose,lease_id,generation,snapshot_version,agent_high_water,
		fixture_revision,profile_sha256,restore_policy,recovery_plan_id,issued_at,expires_at)
		VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13,$14)`,
		challenge.ID, challenge.Nonce, challenge.BoardID, challenge.Purpose,
		nullable(challenge.LeaseID), int64(challenge.Generation), int64(challenge.SnapshotVersion),
		int64(challenge.AgentHighWater), challenge.FixtureRevision, challenge.ProfileSHA256,
		challenge.RestorePolicy, nullable(challenge.RecoveryPlanID), challenge.IssuedAt, challenge.ExpiresAt)
	if err != nil {
		return NeutralChallenge{}, fmt.Errorf("%w: persist challenge: %v", ErrUnavailable, err)
	}
	if err := appendAudit(ctx, tx, actor.id, "board.neutral_challenge.issued", "board", actor.boardID,
		"ok", "", "", "", map[string]any{"challenge_id": challenge.ID, "purpose": purpose, "snapshot_version": snapshot.Version}); err != nil {
		return NeutralChallenge{}, fmt.Errorf("%w: challenge audit: %v", ErrUnavailable, err)
	}
	if err := tx.Commit(ctx); err != nil {
		return NeutralChallenge{}, fmt.Errorf("%w: challenge commit: %v", ErrUnavailable, err)
	}
	return challenge, nil
}

// TickBoard is a server-clock maintenance transition. It never accepts an
// HTTP principal or a user-selected priority.
func (s *Store) TickBoard(ctx context.Context, boardID string, expectedVersion uint64, now time.Time) (board.Snapshot, []board.Event, error) {
	return s.applyBoardCommand(ctx, BoardActor{id: "ra8ci-server", kind: "system", role: "system", boardID: boardID},
		board.Tick{Actor: "ra8ci-server"}, expectedVersion, nil, nil, now)
}

// ApplyBoardCommand serializes one reducer transition with all events, audit,
// and typed board projections. If Apply rejects a command after expiring a
// lease, the expiry and denial still commit before the command error returns.
func (s *Store) ApplyBoardCommand(ctx context.Context, actor BoardActor, command board.Command, expectedVersion uint64, neutral *NeutralSubmission, verifier NeutralReceiptVerifier, now time.Time) (board.Snapshot, []board.Event, error) {
	if actor.kind == "system" {
		return board.Snapshot{}, nil, ErrDenied
	}
	return s.applyBoardCommand(ctx, actor, command, expectedVersion, neutral, verifier, now)
}

func (s *Store) applyBoardCommand(ctx context.Context, actor BoardActor, command board.Command, expectedVersion uint64, neutral *NeutralSubmission, verifier NeutralReceiptVerifier, now time.Time) (board.Snapshot, []board.Event, error) {
	if s == nil || s.pool == nil || actor.id == "" || !validBoardID(actor.boardID) || command == nil || now.IsZero() || expectedVersion > math.MaxInt64 {
		return board.Snapshot{}, nil, fmt.Errorf("%w: board transition arguments", ErrInvalid)
	}
	tx, err := s.pool.BeginTx(ctx, pgx.TxOptions{IsoLevel: pgx.Serializable})
	if err != nil {
		return board.Snapshot{}, nil, fmt.Errorf("%w: begin board transition: %v", ErrUnavailable, err)
	}
	defer func() { _ = tx.Rollback(ctx) }()
	if _, err := tx.Exec(ctx, "SELECT pg_advisory_xact_lock(hashtextextended($1, 1))", actor.boardID); err != nil {
		return board.Snapshot{}, nil, fmt.Errorf("%w: board lock: %v", ErrUnavailable, err)
	}
	if err := revalidateBoardActor(ctx, tx, actor); err != nil {
		return board.Snapshot{}, nil, err
	}
	before, err := loadOrCreateBoard(ctx, tx, actor.boardID)
	if err != nil {
		return board.Snapshot{}, nil, err
	}
	if before.Version != expectedVersion {
		err = fmt.Errorf("%w: board version %d, expected %d", ErrConflict, before.Version, expectedVersion)
		return before, nil, auditBoardDenial(ctx, tx, actor, err)
	}
	trustedCommand, err := authorizeAndBindCommand(actor, before, command, "")
	if err != nil {
		return before, nil, auditBoardDenial(ctx, tx, actor, err)
	}
	var proofError error
	if _, release := trustedCommand.(board.Release); release {
		proof, verifyErr := consumeNeutral(ctx, tx, before, "release", neutral, verifier)
		proofError = verifyErr
		trustedCommand, _ = authorizeAndBindCommand(actor, before, command, proof)
	} else if _, recovery := trustedCommand.(board.CompleteRecovery); recovery {
		proof, verifyErr := consumeNeutral(ctx, tx, before, "recovery", neutral, verifier)
		proofError = verifyErr
		trustedCommand, _ = authorizeAndBindCommand(actor, before, command, proof)
	} else if neutral != nil {
		return before, nil, auditBoardDenial(ctx, tx, actor, ErrDenied)
	}
	var missingNeutralError error
	if release, ok := trustedCommand.(board.Release); ok && release.NeutralReceipt == "" {
		missingNeutralError = &board.Error{Code: board.RecoveryNecessary, Detail: "release requires verified neutral receipt"}
	}
	after, events, commandErr := board.Apply(before, trustedCommand, now.UTC())
	if err := board.Validate(after); err != nil {
		return before, nil, fmt.Errorf("%w: reducer returned invalid board: %v", ErrConflict, err)
	}
	if err := validateBoardSQLBounds(after); err != nil {
		return before, nil, err
	}
	if len(events) > 0 {
		if after.Version != before.Version+1 {
			return before, nil, fmt.Errorf("%w: reducer event without version advance", ErrConflict)
		}
		if err := persistBoardTransition(ctx, tx, before, after, trustedCommand, events); err != nil {
			return before, nil, err
		}
	} else if after.Version != before.Version {
		return before, nil, fmt.Errorf("%w: reducer advanced version without events", ErrConflict)
	}
	if err := tx.Commit(ctx); err != nil {
		if isSerializationFailure(err) {
			return before, nil, fmt.Errorf("%w: concurrent board transition: %v", ErrConflict, err)
		}
		return before, nil, fmt.Errorf("%w: commit board transition: %v", ErrUnavailable, err)
	}
	if commandErr == nil && missingNeutralError != nil {
		return after, events, missingNeutralError
	}
	if proofError != nil && commandErr == nil {
		return after, events, proofError
	}
	return after, events, commandErr
}

// GetBoard returns a consistent snapshot. It never creates a board on read.
func (s *Store) GetBoard(ctx context.Context, boardID string) (board.Snapshot, error) {
	if !validBoardID(boardID) {
		return board.Snapshot{}, fmt.Errorf("%w: board ID", ErrInvalid)
	}
	var version int64
	var raw []byte
	err := s.pool.QueryRow(ctx, "SELECT version, state FROM board_snapshots WHERE board_id=$1", boardID).Scan(&version, &raw)
	if errors.Is(err, pgx.ErrNoRows) {
		return board.Snapshot{}, ErrNotFound
	}
	if err != nil {
		return board.Snapshot{}, fmt.Errorf("%w: board read: %v", ErrUnavailable, err)
	}
	var snapshot board.Snapshot
	if err := json.Unmarshal(raw, &snapshot); err != nil || snapshot.BoardID != boardID || snapshot.Version != uint64(version) {
		return board.Snapshot{}, fmt.Errorf("%w: board snapshot is corrupt", ErrUnavailable)
	}
	if err := board.Validate(snapshot); err != nil {
		return board.Snapshot{}, fmt.Errorf("%w: board snapshot is invalid: %v", ErrUnavailable, err)
	}
	return snapshot, nil
}

func validBoardID(id string) bool {
	return len(id) > 0 && len(id) <= 128 && strings.TrimSpace(id) == id
}

func revalidateBoardActor(ctx context.Context, tx pgx.Tx, actor BoardActor) error {
	if actor.kind == "system" && actor.id == "ra8ci-server" && actor.role == "system" {
		return nil
	}
	if actor.certificate == "" || actor.repository == "" {
		return ErrDenied
	}
	var id string
	err := tx.QueryRow(ctx, `SELECT p.principal_id FROM api_principals p
		JOIN api_grants g ON g.principal_id=p.principal_id
		WHERE p.cert_sha256=$1 AND p.principal_id=$2 AND p.kind=$3
		AND p.revoked_at IS NULL AND p.expires_at>clock_timestamp()
		AND g.repository=$4 AND g.role=$5
		AND (g.board_id=$6 OR (g.board_id='' AND g.role='operator'))
		LIMIT 1`, actor.certificate, actor.id, actor.kind,
		actor.repository, actor.role, actor.boardID).Scan(&id)
	if errors.Is(err, pgx.ErrNoRows) {
		return ErrDenied
	}
	if isSerializationFailure(err) {
		return fmt.Errorf("%w: board actor grant changed concurrently: %v", ErrConflict, err)
	}
	if err != nil {
		return fmt.Errorf("%w: revalidate board actor: %v", ErrUnavailable, err)
	}
	return nil
}

func loadOrCreateBoard(ctx context.Context, tx pgx.Tx, boardID string) (board.Snapshot, error) {
	var version int64
	var raw []byte
	err := tx.QueryRow(ctx, "SELECT version, state FROM board_snapshots WHERE board_id=$1 FOR UPDATE", boardID).Scan(&version, &raw)
	if errors.Is(err, pgx.ErrNoRows) {
		var existing int
		err = tx.QueryRow(ctx, "SELECT COUNT(*) FROM boards WHERE id=$1", boardID).Scan(&existing)
		if err != nil {
			return board.Snapshot{}, fmt.Errorf("%w: board bootstrap check: %v", ErrUnavailable, err)
		}
		if existing != 0 {
			return board.Snapshot{}, fmt.Errorf("%w: typed board exists without reducer snapshot", ErrConflict)
		}
		initial, newErr := board.New(boardID)
		if newErr != nil {
			return board.Snapshot{}, newErr
		}
		encoded, newErr := json.Marshal(initial)
		if newErr != nil {
			return board.Snapshot{}, fmt.Errorf("%w: encode initial board: %v", ErrUnavailable, newErr)
		}
		if _, newErr = tx.Exec(ctx, "INSERT INTO boards (id, generation, state, version) VALUES ($1,0,'available',0)", boardID); newErr != nil {
			return board.Snapshot{}, fmt.Errorf("%w: create board: %v", ErrUnavailable, newErr)
		}
		if _, newErr = tx.Exec(ctx, "INSERT INTO board_snapshots (board_id, version, state) VALUES ($1,0,$2)", boardID, encoded); newErr != nil {
			return board.Snapshot{}, fmt.Errorf("%w: create board snapshot: %v", ErrUnavailable, newErr)
		}
		return initial, nil
	}
	if err != nil {
		if isSerializationFailure(err) {
			return board.Snapshot{}, fmt.Errorf("%w: concurrent board snapshot update: %v", ErrConflict, err)
		}
		return board.Snapshot{}, fmt.Errorf("%w: load board snapshot: %v", ErrUnavailable, err)
	}
	var snapshot board.Snapshot
	if err := json.Unmarshal(raw, &snapshot); err != nil || snapshot.BoardID != boardID || snapshot.Version != uint64(version) {
		return board.Snapshot{}, fmt.Errorf("%w: board snapshot is corrupt", ErrUnavailable)
	}
	if err := board.Validate(snapshot); err != nil {
		return board.Snapshot{}, fmt.Errorf("%w: board snapshot is invalid: %v", ErrUnavailable, err)
	}
	return snapshot, nil
}

func authorizeAndBindCommand(actor BoardActor, before board.Snapshot, command board.Command, proof string) (board.Command, error) {
	if actor.boardID != before.BoardID {
		return nil, ErrDenied
	}
	switch c := command.(type) {
	case board.Enqueue:
		if c.Waiter.Holder != actor.id || !ValidID(c.Waiter.ID) || !ValidID(c.Waiter.LeaseID) || !classAllowed(actor, c.Waiter.Class) {
			return nil, ErrDenied
		}
		c.Actor = actor.id
		return c, nil
	case board.CancelWaiter:
		if actor.role != "operator" && !ownsWaiter(before, c.WaiterID, actor.id) {
			return nil, ErrDenied
		}
		c.Actor = actor.id
		return c, nil
	case board.RequestYield:
		if !ownsWaiter(before, c.WaiterID, actor.id) {
			return nil, ErrDenied
		}
		c.Actor = actor.id
		return c, nil
	case board.BeginDrain:
		if !ownsLease(before, actor.id) {
			return nil, ErrDenied
		}
		c.Actor = actor.id
		return c, nil
	case board.Release:
		if !ownsLease(before, actor.id) {
			return nil, ErrDenied
		}
		c.Actor, c.NeutralReceipt = actor.id, proof
		return c, nil
	case board.Extend:
		if !ownsLease(before, actor.id) {
			return nil, ErrDenied
		}
		c.Actor = actor.id
		return c, nil
	case board.AcknowledgeGrant:
		if actor.kind != "board_agent" || actor.role != "board_agent" {
			return nil, ErrDenied
		}
		c.Actor = actor.id
		return c, nil
	case board.ObserveAgentGeneration:
		if actor.kind != "board_agent" || actor.role != "board_agent" {
			return nil, ErrDenied
		}
		c.Actor = actor.id
		return c, nil
	case board.AgentUnavailable:
		if actor.kind != "system" && actor.kind != "board_agent" {
			return nil, ErrDenied
		}
		c.Actor = actor.id
		return c, nil
	case board.BeginRecovery:
		if actor.role != "operator" {
			return nil, ErrDenied
		}
		c.Actor = actor.id
		return c, nil
	case board.CompleteRecovery:
		if actor.role != "operator" {
			return nil, ErrDenied
		}
		// The request body cannot claim a higher agent generation. The
		// board-agent observation is a separate authenticated transition.
		c.Actor, c.NeutralReceipt, c.AgentHighWater = actor.id, proof, before.AgentHighWater
		return c, nil
	case board.Quarantine:
		if actor.role != "operator" && actor.kind != "system" {
			return nil, ErrDenied
		}
		c.Actor = actor.id
		return c, nil
	case board.Tick:
		if actor.kind != "system" {
			return nil, ErrDenied
		}
		c.Actor = actor.id
		return c, nil
	default:
		return nil, fmt.Errorf("%w: unsupported board command", ErrInvalid)
	}
}

func classAllowed(actor BoardActor, class board.Class) bool {
	switch class {
	case board.ClassHuman:
		return actor.kind == "human" && (actor.role == "board_human" || actor.role == "operator")
	case board.ClassCI:
		return actor.kind == "github_app" && actor.role == "submitter"
	case board.ClassAI:
		return actor.kind == "agent" && actor.role == "submitter"
	default:
		return false
	}
}

func ownsWaiter(snapshot board.Snapshot, waiterID, actorID string) bool {
	for _, waiter := range snapshot.Queue {
		if waiter.ID == waiterID && waiter.Holder == actorID {
			return true
		}
	}
	return false
}

func ownsLease(snapshot board.Snapshot, actorID string) bool {
	return snapshot.Lease != nil && snapshot.Lease.Holder == actorID
}

func validateBoardSQLBounds(snapshot board.Snapshot) error {
	if snapshot.Version > math.MaxInt64 || snapshot.Generation > math.MaxInt64 || snapshot.AgentHighWater > math.MaxInt64 || snapshot.NextSequence > math.MaxInt64 {
		return fmt.Errorf("%w: board counter exceeds PostgreSQL bigint", ErrConflict)
	}
	if snapshot.Lease != nil && snapshot.Lease.Generation > math.MaxInt64 {
		return fmt.Errorf("%w: lease generation exceeds PostgreSQL bigint", ErrConflict)
	}
	for _, waiter := range snapshot.Queue {
		if waiter.Sequence > math.MaxInt64 {
			return fmt.Errorf("%w: waiter sequence exceeds PostgreSQL bigint", ErrConflict)
		}
	}
	return nil
}

func persistBoardTransition(ctx context.Context, tx pgx.Tx, before, after board.Snapshot, command board.Command, events []board.Event) error {
	encoded, err := json.Marshal(after)
	if err != nil {
		return fmt.Errorf("%w: encode board: %v", ErrUnavailable, err)
	}
	tag, err := tx.Exec(ctx, `UPDATE board_snapshots SET version=$2, state=$3,
		updated_at=clock_timestamp() WHERE board_id=$1 AND version=$4`,
		after.BoardID, int64(after.Version), encoded, int64(before.Version))
	if err != nil || tag.RowsAffected() != 1 {
		return fmt.Errorf("%w: board snapshot CAS: %v", ErrConflict, err)
	}
	if err := projectBoard(ctx, tx, before, after, command, events); err != nil {
		return err
	}
	var seq int64
	if err := tx.QueryRow(ctx, "SELECT COALESCE(MAX(event_seq),0) FROM board_events WHERE board_id=$1", after.BoardID).Scan(&seq); err != nil {
		return fmt.Errorf("%w: board event sequence: %v", ErrUnavailable, err)
	}
	for _, event := range events {
		seq++
		id, err := NewID()
		if err != nil {
			return fmt.Errorf("%w: board event ID: %v", ErrUnavailable, err)
		}
		_, err = tx.Exec(ctx, `INSERT INTO board_events
			(board_id,event_seq,id,snapshot_version,kind,happened_at,actor_id,waiter_id,lease_id,generation,reason)
			VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11)`,
			after.BoardID, seq, id, int64(after.Version), string(event.Kind), event.At,
			event.Actor, nullable(event.WaiterID), nullable(event.LeaseID), int64(event.Generation), event.Reason)
		if err != nil {
			return fmt.Errorf("%w: board event insert: %v", ErrUnavailable, err)
		}
		if err := appendAudit(ctx, tx, event.Actor, "board."+string(event.Kind), "board", after.BoardID,
			"recorded", string(before.Phase), string(after.Phase), "", map[string]any{
				"event_id": id, "lease_id": event.LeaseID, "generation": event.Generation,
				"reason": event.Reason,
			}); err != nil {
			return fmt.Errorf("%w: board audit insert: %v", ErrUnavailable, err)
		}
	}
	return nil
}

func projectBoard(ctx context.Context, tx pgx.Tx, before, after board.Snapshot, command board.Command, events []board.Event) error {
	state := "available"
	switch after.Phase {
	case board.GrantPending, board.Active, board.YieldRequested, board.Draining:
		state = "held"
	case board.RecoveryRequired, board.Recovering:
		state = "recovery_required"
	case board.Quarantined:
		state = "quarantined"
	}
	tag, err := tx.Exec(ctx, `UPDATE boards SET generation=$2, state=$3,
		recovery_required=$4, version=$5 WHERE id=$1`, after.BoardID,
		int64(after.Generation), state, state == "recovery_required" || state == "quarantined", int64(after.Version))
	if err != nil || tag.RowsAffected() != 1 {
		return fmt.Errorf("%w: project board state: %v", ErrConflict, err)
	}
	if enqueue, ok := command.(board.Enqueue); ok {
		for _, event := range events {
			if event.Kind == board.WaitQueued && event.WaiterID == enqueue.Waiter.ID {
				if err := insertBoardWaiter(ctx, tx, after.BoardID, enqueue.Waiter, event.At); err != nil {
					return err
				}
				break
			}
		}
	}
	for _, waiter := range after.Queue {
		if err := verifyBoardWaiter(ctx, tx, after.BoardID, waiter); err != nil {
			return err
		}
	}
	for _, waiter := range before.Queue {
		if !queueContains(after.Queue, waiter.ID) && (after.Lease == nil || after.Lease.WaiterID != waiter.ID) {
			tag, err := tx.Exec(ctx, `UPDATE board_waiters SET state='cancelled', ended_at=clock_timestamp()
				WHERE id=$1 AND board_id=$2 AND actor_id=$3 AND state='waiting'`, waiter.ID, after.BoardID, waiter.Holder)
			if err != nil || tag.RowsAffected() != 1 {
				return fmt.Errorf("%w: project waiter exit: %v", ErrConflict, err)
			}
		}
	}
	if before.Lease != nil && liveBoardPhase(before.Phase) && (after.Lease == nil || before.Lease.ID != after.Lease.ID || !liveBoardPhase(after.Phase)) {
		endReason := "released"
		if after.Phase == board.RecoveryRequired || after.Phase == board.Recovering || after.Phase == board.Quarantined {
			endReason = "recovery_required"
		}
		for _, event := range events {
			if event.Kind == board.LeaseExpired {
				endReason = "expired"
			}
		}
		tag, err := tx.Exec(ctx, `UPDATE board_leases SET state='ended', ended_at=clock_timestamp(),
			end_reason=$6, version=version+1 WHERE id=$1 AND board_id=$2 AND waiter_id=$3
			AND holder_id=$4 AND generation=$5 AND state IN ('pending','active')`,
			before.Lease.ID, after.BoardID, before.Lease.WaiterID, before.Lease.Holder, int64(before.Lease.Generation), endReason)
		if err != nil || tag.RowsAffected() != 1 {
			return fmt.Errorf("%w: project lease end: %v", ErrConflict, err)
		}
	}
	if before.Lease != nil && (after.Lease == nil || before.Lease.ID != after.Lease.ID) {
		tag, err := tx.Exec(ctx, `UPDATE board_sessions SET ended_at=clock_timestamp(),phase=$4
			WHERE board_id=$1 AND lease_id=$2 AND owner_id=$3 AND ended_at IS NULL`,
			after.BoardID, before.Lease.ID, before.Lease.Holder, string(after.Phase))
		if err != nil || tag.RowsAffected() != 1 {
			return fmt.Errorf("%w: project session end: %v", ErrConflict, err)
		}
	}
	if after.Lease != nil && liveBoardPhase(after.Phase) {
		lease := after.Lease
		leaseState := "active"
		if after.Phase == board.GrantPending {
			leaseState = "pending"
		}
		if before.Lease == nil || before.Lease.ID != lease.ID {
			_, err := tx.Exec(ctx, `INSERT INTO board_leases
				(id,board_id,waiter_id,generation,holder_id,priority,reason,requested_duration_seconds,
				granted_at,expires_at,yield_requested_at,state)
				VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12)`, lease.ID, after.BoardID, lease.WaiterID,
				int64(lease.Generation), lease.Holder, boardPriority(lease.Class), lease.Reason,
				ceilSeconds(lease.RequestedDuration), lease.GrantedAt, lease.ExpiresAt,
				nullTime(lease.YieldRequestedAt), leaseState)
			if err != nil {
				return fmt.Errorf("%w: project new lease: %v", ErrConflict, err)
			}
			tag, err = tx.Exec(ctx, `UPDATE board_waiters SET state='granted', ended_at=clock_timestamp()
				WHERE id=$1 AND board_id=$2 AND actor_id=$3 AND state='waiting'`,
				lease.WaiterID, after.BoardID, lease.Holder)
			if err != nil || tag.RowsAffected() != 1 {
				return fmt.Errorf("%w: project granted waiter: %v", ErrConflict, err)
			}
			var revision, profile, policy string
			err = tx.QueryRow(ctx, `SELECT fixture_revision,profile_sha256,restore_policy
				FROM board_fixture_profiles WHERE board_id=$1`, after.BoardID).
				Scan(&revision, &profile, &policy)
			if err != nil {
				return fmt.Errorf("%w: approved fixture required for grant: %v", ErrDenied, err)
			}
			sessionID, err := NewID()
			if err != nil {
				return fmt.Errorf("%w: session ID: %v", ErrUnavailable, err)
			}
			_, err = tx.Exec(ctx, `INSERT INTO board_sessions
				(id,board_id,lease_id,owner_id,fixture_revision,profile_sha256,phase,restore_policy,metadata_version)
				VALUES ($1,$2,$3,$4,$5,$6,$7,$8,1)`,
				sessionID, after.BoardID, lease.ID, lease.Holder, revision, profile, string(after.Phase), policy)
			if err != nil {
				return fmt.Errorf("%w: project new session: %v", ErrConflict, err)
			}
		} else {
			tag, err := tx.Exec(ctx, `UPDATE board_leases SET expires_at=$6,yield_requested_at=$7,
				state=$8,version=version+1 WHERE id=$1 AND board_id=$2 AND waiter_id=$3
				AND holder_id=$4 AND generation=$5 AND state IN ('pending','active')`,
				lease.ID, after.BoardID, lease.WaiterID, lease.Holder, int64(lease.Generation),
				lease.ExpiresAt, nullTime(lease.YieldRequestedAt), leaseState)
			if err != nil || tag.RowsAffected() != 1 {
				return fmt.Errorf("%w: project live lease: %v", ErrConflict, err)
			}
			tag, err = tx.Exec(ctx, `UPDATE board_sessions SET phase=$4 WHERE board_id=$1
				AND lease_id=$2 AND owner_id=$3 AND ended_at IS NULL`,
				after.BoardID, lease.ID, lease.Holder, string(after.Phase))
			if err != nil || tag.RowsAffected() != 1 {
				return fmt.Errorf("%w: project live session: %v", ErrConflict, err)
			}
		}
	}
	for _, event := range events {
		switch event.Kind {
		case board.RecoveryStarted:
			start, ok := command.(board.BeginRecovery)
			if !ok {
				return ErrConflict
			}
			tag, err := tx.Exec(ctx, `INSERT INTO board_recovery_context (board_id,plan_id)
				VALUES ($1,$2) ON CONFLICT (board_id) DO UPDATE SET plan_id=EXCLUDED.plan_id,
				started_at=clock_timestamp(),ended_at=NULL,version=board_recovery_context.version+1
				WHERE board_recovery_context.ended_at IS NOT NULL`, after.BoardID, start.PlanID)
			if err != nil || tag.RowsAffected() != 1 {
				return fmt.Errorf("%w: recovery plan projection: %v", ErrConflict, err)
			}
		case board.RecoveryFinished:
			tag, err := tx.Exec(ctx, `UPDATE board_recovery_context SET ended_at=clock_timestamp(),version=version+1
				WHERE board_id=$1 AND ended_at IS NULL`, after.BoardID)
			if err != nil || tag.RowsAffected() != 1 {
				return fmt.Errorf("%w: recovery plan completion: %v", ErrConflict, err)
			}
		}
	}
	return nil
}

func insertBoardWaiter(ctx context.Context, tx pgx.Tx, boardID string, w board.Waiter, queuedAt time.Time) error {
	if !ValidID(w.ID) || !ValidID(w.LeaseID) || queuedAt.IsZero() {
		return fmt.Errorf("%w: board waiter cannot be projected", ErrConflict)
	}
	_, err := tx.Exec(ctx, `INSERT INTO board_waiters
		(id,board_id,actor_id,priority,reason,requested_duration_seconds,state,requested_at,request_id)
		VALUES ($1,$2,$3,$4,$5,$6,'waiting',$7,$8)`,
		w.ID, boardID, w.Holder, boardPriority(w.Class), w.Reason, ceilSeconds(w.Duration), queuedAt, w.ID)
	if err != nil {
		return fmt.Errorf("%w: project waiter: %v", ErrConflict, err)
	}
	return nil
}

func verifyBoardWaiter(ctx context.Context, tx pgx.Tx, boardID string, w board.Waiter) error {
	var actualBoard, actor, priority, reason, state string
	var duration int64
	err := tx.QueryRow(ctx, `SELECT board_id,actor_id,priority,reason,requested_duration_seconds,state
		FROM board_waiters WHERE id=$1 FOR SHARE`, w.ID).
		Scan(&actualBoard, &actor, &priority, &reason, &duration, &state)
	if err != nil || actualBoard != boardID || actor != w.Holder || priority != boardPriority(w.Class) ||
		reason != w.Reason || duration != ceilSeconds(w.Duration) || state != "waiting" {
		return fmt.Errorf("%w: queue waiter projection mismatch: %v", ErrConflict, err)
	}
	return nil
}

func queueContains(queue []board.Waiter, id string) bool {
	for _, w := range queue {
		if w.ID == id {
			return true
		}
	}
	return false
}

func boardPriority(class board.Class) string {
	switch class {
	case board.ClassHuman:
		return "human"
	case board.ClassCI:
		return "ci"
	default:
		return "agent"
	}
}

func liveBoardPhase(phase board.Phase) bool {
	return phase == board.GrantPending || phase == board.Active || phase == board.YieldRequested || phase == board.Draining
}

func ceilSeconds(d time.Duration) int64 {
	return int64((d + time.Second - 1) / time.Second)
}

func nullTime(t time.Time) any {
	if t.IsZero() {
		return nil
	}
	return t
}

func isSerializationFailure(err error) bool {
	var pgErr *pgconn.PgError
	return errors.As(err, &pgErr) && pgErr.Code == "40001"
}
