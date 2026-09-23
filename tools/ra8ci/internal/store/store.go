// Package store owns transactional PostgreSQL state for the ra8ci control plane.
package store

import (
	"context"
	"crypto/aes"
	"crypto/cipher"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"math"
	"regexp"
	"strings"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/migrations"
	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgxpool"
)

var hexSHA = regexp.MustCompile(`^[0-9a-f]{64}$`)
var commitSHA = regexp.MustCompile(`^[0-9a-f]{40}$`)

type Store struct {
	pool               *pgxpool.Pool
	terraformStateAEAD cipher.AEAD
}

// Open connects with the runtime role. It never runs DDL or silently upgrades
// the schema; migration must happen under a separate credential.
func Open(ctx context.Context, dsn string) (*Store, error) {
	if strings.TrimSpace(dsn) == "" {
		return nil, fmt.Errorf("%w: database DSN is empty", ErrInvalid)
	}
	pool, err := pgxpool.New(ctx, dsn)
	if err != nil {
		return nil, fmt.Errorf("%w: connect: %v", ErrUnavailable, err)
	}
	s := &Store{pool: pool}
	if err := s.CheckSchema(ctx); err != nil {
		pool.Close()
		return nil, err
	}
	return s, nil
}

func (s *Store) Close() {
	if s != nil && s.pool != nil {
		s.pool.Close()
	}
}

// CheckSchema refuses both an uninitialized database and a future version.
func (s *Store) CheckSchema(ctx context.Context) error {
	if s == nil || s.pool == nil {
		return fmt.Errorf("%w: store is nil", ErrUnavailable)
	}
	var version, count int
	err := s.pool.QueryRow(ctx, "SELECT COALESCE(MAX(version), 0), COUNT(*) FROM schema_migrations").Scan(&version, &count)
	if err != nil {
		return fmt.Errorf("%w: schema ledger: %v", ErrUnavailable, err)
	}
	if version != migrations.CurrentVersion() || count != version {
		return fmt.Errorf("%w: incompatible schema version %d (rows %d, expected %d)", ErrUnavailable, version, count, migrations.CurrentVersion())
	}
	for _, relation := range []string{"audit", "board_events", "run_events", "local_runs", "local_run_steps", "hil_observations"} {
		var canUpdate, canDelete, canTruncate bool
		err := s.pool.QueryRow(ctx, `SELECT has_table_privilege(current_user,$1,'UPDATE'),
			has_table_privilege(current_user,$1,'DELETE'),
			has_table_privilege(current_user,$1,'TRUNCATE')`, relation).
			Scan(&canUpdate, &canDelete, &canTruncate)
		if err != nil {
			return fmt.Errorf("%w: inspect %s runtime privileges: %v", ErrUnavailable, relation, err)
		}
		if canUpdate || canDelete || canTruncate {
			return fmt.Errorf("%w: runtime role can mutate append-only %s", ErrUnavailable, relation)
		}
	}
	for _, relation := range []string{"api_principals", "api_grants", "agents", "board_fixture_profiles", "schema_migrations"} {
		var canInsert, canUpdate, canDelete, canTruncate bool
		err := s.pool.QueryRow(ctx, `SELECT has_table_privilege(current_user,$1,'INSERT'),
			has_table_privilege(current_user,$1,'UPDATE'),
			has_table_privilege(current_user,$1,'DELETE'),
			has_table_privilege(current_user,$1,'TRUNCATE')`, relation).
			Scan(&canInsert, &canUpdate, &canDelete, &canTruncate)
		if err != nil {
			return fmt.Errorf("%w: inspect %s runtime privileges: %v", ErrUnavailable, relation, err)
		}
		if canInsert || canUpdate || canDelete || canTruncate {
			return fmt.Errorf("%w: runtime role can mutate authority table %s", ErrUnavailable, relation)
		}
	}
	for _, relation := range []string{"runner_vms", "runner_vm_operations", "runner_vm_terraform_states"} {
		var canDelete, canTruncate bool
		err := s.pool.QueryRow(ctx, `SELECT has_table_privilege(current_user,$1,'DELETE'),
			has_table_privilege(current_user,$1,'TRUNCATE')`, relation).
			Scan(&canDelete, &canTruncate)
		if err != nil {
			return fmt.Errorf("%w: inspect %s runtime privileges: %v", ErrUnavailable, relation, err)
		}
		if canDelete || canTruncate {
			return fmt.Errorf("%w: runtime role can erase runner VM ledger %s", ErrUnavailable, relation)
		}
	}
	return nil
}

// Health proves a write and an append-only audit commit, not just a socket.
func (s *Store) Health(ctx context.Context) error {
	if err := s.CheckSchema(ctx); err != nil {
		return err
	}
	tx, err := s.pool.Begin(ctx)
	if err != nil {
		return fmt.Errorf("%w: readiness transaction: %v", ErrUnavailable, err)
	}
	defer func() { _ = tx.Rollback(ctx) }()
	if err := appendAudit(ctx, tx, "ra8ci-server", "server.readiness", "server", "control", "ok", "", "", "", nil); err != nil {
		return fmt.Errorf("%w: readiness audit: %v", ErrUnavailable, err)
	}
	if err := tx.Commit(ctx); err != nil {
		return fmt.Errorf("%w: readiness commit: %v", ErrUnavailable, err)
	}
	return nil
}

// AuthorizeCertificate maps a verified mTLS leaf to an active, scoped grant.
// The HTTP layer is responsible for requiring a verified certificate chain.
func (s *Store) AuthorizeCertificate(ctx context.Context, certDER []byte, repository, role string) (string, error) {
	if len(certDER) == 0 || repository == "" || (role != "read" && role != "submit" && role != "terraform_state") {
		return "", fmt.Errorf("%w: invalid certificate scope request", ErrInvalid)
	}
	sum := sha256.Sum256(certDER)
	var principal string
	err := s.pool.QueryRow(ctx, `SELECT p.principal_id FROM api_principals p
		JOIN api_grants g ON g.principal_id = p.principal_id
		WHERE p.cert_sha256 = $1 AND p.revoked_at IS NULL AND p.expires_at > clock_timestamp()
		AND g.repository = $2 AND (
			($3 = 'read' AND g.role IN ('observer', 'submitter', 'operator')) OR
			($3 = 'submit' AND g.role IN ('submitter', 'operator')) OR
			($3 = 'terraform_state' AND g.role = 'terraform_state')
		) LIMIT 1`, hex.EncodeToString(sum[:]), repository, role).Scan(&principal)
	if errors.Is(err, pgx.ErrNoRows) {
		return "", ErrDenied
	}
	if err != nil {
		return "", fmt.Errorf("%w: authorize certificate: %v", ErrUnavailable, err)
	}
	return principal, nil
}

func (s *Store) LookupRunRepository(ctx context.Context, id string) (string, error) {
	if !ValidID(id) {
		return "", fmt.Errorf("%w: run ID", ErrInvalid)
	}
	var repository string
	err := s.pool.QueryRow(ctx, "SELECT repository FROM runs WHERE id = $1", id).Scan(&repository)
	if errors.Is(err, pgx.ErrNoRows) {
		return "", ErrNotFound
	}
	if err != nil {
		return "", fmt.Errorf("%w: run lookup: %v", ErrUnavailable, err)
	}
	return repository, nil
}

// AuditDenied commits an authorization denial before the HTTP layer responds.
func (s *Store) AuditDenied(ctx context.Context, actor, action, target string) error {
	if actor == "" || action == "" || target == "" {
		return fmt.Errorf("%w: denial audit fields", ErrInvalid)
	}
	tx, err := s.pool.Begin(ctx)
	if err != nil {
		return fmt.Errorf("%w: begin denial audit: %v", ErrUnavailable, err)
	}
	defer func() { _ = tx.Rollback(ctx) }()
	if err := appendAudit(ctx, tx, actor, action, "api", target, "denied", "", "", "", nil); err != nil {
		return fmt.Errorf("%w: denial audit: %v", ErrUnavailable, err)
	}
	if err := tx.Commit(ctx); err != nil {
		return fmt.Errorf("%w: denial commit: %v", ErrUnavailable, err)
	}
	return nil
}

func appendAudit(ctx context.Context, tx pgx.Tx, actor, action, targetType, targetID, outcome, previous, next, runID string, reason any) error {
	id, err := NewID()
	if err != nil {
		return fmt.Errorf("%w: %v", errEntropy, err)
	}
	if reason == nil {
		reason = map[string]any{}
	}
	data, err := json.Marshal(reason)
	if err != nil {
		return err
	}
	var correlation any
	if runID != "" {
		correlation = runID
	}
	_, err = tx.Exec(ctx, `INSERT INTO audit
		(id, actor_id, action, target_type, target_id, outcome, previous_state, new_state, correlation_run_id, reason)
		VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10)`, id, actor, action, targetType, targetID, outcome, nullable(previous), nullable(next), correlation, data)
	return err
}

func appendEvent(ctx context.Context, tx pgx.Tx, runID, kind string, data any) error {
	id, err := NewID()
	if err != nil {
		return fmt.Errorf("%w: %v", errEntropy, err)
	}
	encoded, err := json.Marshal(data)
	if err != nil {
		return err
	}
	_, err = tx.Exec(ctx, `INSERT INTO run_events (run_id, event_seq, id, kind, data)
		SELECT $1, COALESCE(MAX(event_seq), 0) + 1, $2, $3, $4
		FROM run_events WHERE run_id = $1`, runID, id, kind, encoded)
	return err
}

func nullable(s string) any {
	if s == "" {
		return nil
	}
	return s
}

func validObject(raw json.RawMessage) (json.RawMessage, bool) {
	if len(raw) == 0 {
		return json.RawMessage(`{}`), true
	}
	var value any
	if err := json.Unmarshal(raw, &value); err != nil {
		return nil, false
	}
	if _, ok := value.(map[string]any); !ok {
		return nil, false
	}
	return raw, true
}

func validHostFacts(in StartAttemptInput) bool {
	if !ValidID(in.TaskID) || (in.AgentID != "" && !ValidID(in.AgentID)) || in.ActorID == "" || in.Engine == "" || len(in.Engine) > 64 || in.HostCores < 1 || in.HostRAMBytes < 1 || in.HostLoad < 0 || math.IsNaN(in.HostLoad) || math.IsInf(in.HostLoad, 0) {
		return false
	}
	_, ok := validObject(in.HostFacts)
	return ok
}

// WaitForReady checks the same persisted readiness boundary under a deadline.
func (s *Store) WaitForReady(ctx context.Context, interval time.Duration) error {
	if interval <= 0 {
		return fmt.Errorf("%w: retry interval", ErrInvalid)
	}
	ticker := time.NewTicker(interval)
	defer ticker.Stop()
	for {
		if err := s.Health(ctx); err == nil {
			return nil
		}
		select {
		case <-ctx.Done():
			return ctx.Err()
		case <-ticker.C:
		}
	}
}

// OpenWithTerraformStateKey opens a store that can serve encrypted Terraform
// state. The key must be a dedicated 256-bit secret loaded from the secret
// manager; it is never stored in PostgreSQL.
func OpenWithTerraformStateKey(ctx context.Context, dsn string, key []byte) (*Store, error) {
	if len(key) != 32 {
		return nil, fmt.Errorf("%w: Terraform state encryption key must be 32 bytes", ErrInvalid)
	}
	block, err := aes.NewCipher(key)
	if err != nil {
		return nil, fmt.Errorf("%w: initialize Terraform state cipher: %v", ErrInvalid, err)
	}
	aead, err := cipher.NewGCM(block)
	if err != nil {
		return nil, fmt.Errorf("%w: initialize Terraform state AEAD: %v", ErrInvalid, err)
	}
	s, err := Open(ctx, dsn)
	if err != nil {
		return nil, err
	}
	s.terraformStateAEAD = aead
	return s, nil
}
