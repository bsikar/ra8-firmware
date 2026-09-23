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

// privilegeRule states which privileges the runtime role must not hold on a
// group of relations, and what holding one of them means.
type privilegeRule struct {
	relations  []string
	privileges []string
	// violation is formatted with the offending relation name.
	violation string
}

// runtimePrivilegeRules is the whole contract between the runtime role and the
// schema: the ledgers it may append to but never rewrite, the authority tables
// it may only read, and the runner VM ledger it may update but never erase.
var runtimePrivilegeRules = []privilegeRule{
	{
		relations:  []string{"audit", "board_events", "run_events", "local_runs", "local_run_steps"},
		privileges: []string{"UPDATE", "DELETE", "TRUNCATE"},
		violation:  "runtime role can mutate append-only %s",
	},
	{
		relations:  []string{"api_principals", "api_grants", "agents", "board_fixture_profiles", "schema_migrations"},
		privileges: []string{"INSERT", "UPDATE", "DELETE", "TRUNCATE"},
		violation:  "runtime role can mutate authority table %s",
	},
	{
		relations:  []string{"runner_vms", "runner_vm_operations", "runner_vm_terraform_states"},
		privileges: []string{"DELETE", "TRUNCATE"},
		violation:  "runtime role can erase runner VM ledger %s",
	},
}

// CheckSchema refuses both an uninitialized database and a future version.
func (s *Store) CheckSchema(ctx context.Context) error {
	if s == nil || s.pool == nil {
		return fmt.Errorf("%w: store is nil", ErrUnavailable)
	}
	if err := s.checkSchemaVersion(ctx); err != nil {
		return err
	}
	for _, rule := range runtimePrivilegeRules {
		if err := s.checkRuntimePrivileges(ctx, rule); err != nil {
			return err
		}
	}
	return nil
}

// checkSchemaVersion requires the ledger to hold exactly the migrations this
// binary knows about: one row per version, and no version beyond ours.
func (s *Store) checkSchemaVersion(ctx context.Context) error {
	var version, count int
	err := s.pool.QueryRow(ctx, "SELECT COALESCE(MAX(version), 0), COUNT(*) FROM schema_migrations").Scan(&version, &count)
	if err != nil {
		return fmt.Errorf("%w: schema ledger: %v", ErrUnavailable, err)
	}
	if version != migrations.CurrentVersion() || count != version {
		return fmt.Errorf("%w: incompatible schema version %d (rows %d, expected %d)", ErrUnavailable, version, count, migrations.CurrentVersion())
	}
	return nil
}

// checkRuntimePrivileges fails on the first relation where the runtime role
// holds any privilege the rule forbids.
func (s *Store) checkRuntimePrivileges(ctx context.Context, rule privilegeRule) error {
	for _, relation := range rule.relations {
		var forbidden bool
		err := s.pool.QueryRow(ctx,
			`SELECT bool_or(has_table_privilege(current_user, $1, privilege))
			FROM unnest($2::text[]) AS privilege`,
			relation, rule.privileges).Scan(&forbidden)
		if err != nil {
			return fmt.Errorf("%w: inspect %s runtime privileges: %v", ErrUnavailable, relation, err)
		}
		if forbidden {
			return fmt.Errorf("%w: "+rule.violation, ErrUnavailable, relation)
		}
	}
	return nil
}

// withTx runs fn inside a transaction and commits only if fn succeeds. The
// rollback is unconditional; committing first makes it a no-op. stage names the
// step in any error, so a caller reads which boundary failed.
func (s *Store) withTx(ctx context.Context, stage string, fn func(pgx.Tx) error) error {
	tx, err := s.pool.Begin(ctx)
	if err != nil {
		return fmt.Errorf("%w: begin %s: %v", ErrUnavailable, stage, err)
	}
	defer func() { _ = tx.Rollback(ctx) }()
	if err := fn(tx); err != nil {
		return err
	}
	if err := tx.Commit(ctx); err != nil {
		return fmt.Errorf("%w: commit %s: %v", ErrUnavailable, stage, err)
	}
	return nil
}

// Health proves a write and an append-only audit commit, not just a socket.
func (s *Store) Health(ctx context.Context) error {
	if err := s.CheckSchema(ctx); err != nil {
		return err
	}
	return s.withTx(ctx, "readiness", func(tx pgx.Tx) error {
		if err := appendAudit(ctx, tx, "ra8ci-server", "server.readiness", "server", "control", "ok", "", "", "", nil); err != nil {
			return fmt.Errorf("%w: readiness audit: %v", ErrUnavailable, err)
		}
		return nil
	})
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
	return s.withTx(ctx, "denial audit", func(tx pgx.Tx) error {
		if err := appendAudit(ctx, tx, actor, action, "api", target, "denied", "", "", "", nil); err != nil {
			return fmt.Errorf("%w: denial audit: %v", ErrUnavailable, err)
		}
		return nil
	})
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

// validHostFacts rejects an attempt whose identifiers, engine name, or
// reported host shape could not have come from a healthy agent.
func validHostFacts(in StartAttemptInput) bool {
	switch {
	case !ValidID(in.TaskID):
		return false
	case in.AgentID != "" && !ValidID(in.AgentID):
		return false
	case in.ActorID == "":
		return false
	case in.Engine == "" || len(in.Engine) > 64:
		return false
	case in.HostCores < 1 || in.HostRAMBytes < 1:
		return false
	case !validLoad(in.HostLoad):
		return false
	}
	_, ok := validObject(in.HostFacts)
	return ok
}

// validLoad accepts a real, non-negative load average and nothing else.
func validLoad(load float64) bool {
	return load >= 0 && !math.IsNaN(load) && !math.IsInf(load, 0)
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
