package store

import (
	"context"
	"crypto/cipher"
	"crypto/rand"
	"crypto/sha256"
	"database/sql"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"regexp"

	"github.com/jackc/pgx/v5"
)

const maxTerraformStateBytes = 16 << 20

var terraformLineagePattern = regexp.MustCompile("^[0-9a-f]{8}-[0-9a-f]{4}-[1-8][0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$")

type TerraformStateLock struct {
	ID        string `json:"ID"`
	Operation string `json:"Operation"`
	Info      string `json:"Info"`
	Who       string `json:"Who"`
	Version   string `json:"Version"`
	Created   string `json:"Created"`
	Path      string `json:"Path"`
}

type terraformStateHeader struct {
	Version          int    `json:"version"`
	TerraformVersion string `json:"terraform_version"`
	Serial           int64  `json:"serial"`
	Lineage          string `json:"lineage"`
}

func (s *Store) stateAEAD() (cipher.AEAD, error) {
	if s == nil || s.pool == nil || s.terraformStateAEAD == nil {
		return nil, fmt.Errorf("%w: Terraform state encryption is not configured", ErrUnavailable)
	}
	return s.terraformStateAEAD, nil
}

func terraformStateAAD(reservationID string) []byte {
	return []byte("ra8ci-runner-terraform-state:" + reservationID)
}

func parseTerraformState(body []byte) (terraformStateHeader, string, error) {
	if len(body) == 0 || len(body) > maxTerraformStateBytes {
		return terraformStateHeader{}, "", ErrInvalid
	}
	var header terraformStateHeader
	if err := json.Unmarshal(body, &header); err != nil || header.Version != 4 ||
		header.Serial < 0 || !terraformLineagePattern.MatchString(header.Lineage) || len(header.TerraformVersion) == 0 || len(header.TerraformVersion) > 64 {
		return terraformStateHeader{}, "", fmt.Errorf("%w: invalid Terraform state envelope", ErrInvalid)
	}
	sum := sha256.Sum256(body)
	return header, hex.EncodeToString(sum[:]), nil
}

func (s *Store) sealTerraformState(reservationID string, body []byte) ([]byte, error) {
	aead, err := s.stateAEAD()
	if err != nil {
		return nil, err
	}
	nonce := make([]byte, aead.NonceSize())
	if _, err := rand.Read(nonce); err != nil {
		return nil, fmt.Errorf("%w: generate Terraform state nonce: %v", ErrUnavailable, err)
	}
	return append(nonce, aead.Seal(nil, nonce, body, terraformStateAAD(reservationID))...), nil
}

func (s *Store) openTerraformState(reservationID string, ciphertext []byte) ([]byte, error) {
	aead, err := s.stateAEAD()
	if err != nil {
		return nil, err
	}
	if len(ciphertext) <= aead.NonceSize()+aead.Overhead() {
		return nil, fmt.Errorf("%w: encrypted Terraform state is truncated", ErrUnavailable)
	}
	nonce := ciphertext[:aead.NonceSize()]
	body, err := aead.Open(nil, nonce, ciphertext[aead.NonceSize():], terraformStateAAD(reservationID))
	if err != nil {
		return nil, fmt.Errorf("%w: decrypt Terraform state: %v", ErrUnavailable, err)
	}
	return body, nil
}

func lockFromJSON(raw []byte) (TerraformStateLock, error) {
	if len(raw) == 0 || len(raw) > 32<<10 {
		return TerraformStateLock{}, ErrInvalid
	}
	var lock TerraformStateLock
	if err := json.Unmarshal(raw, &lock); err != nil || !ValidID(lock.ID) || lock.Operation == "" || len(lock.Operation) > 128 || len(lock.Who) > 256 || len(lock.Version) > 64 || len(lock.Path) > 1024 {
		return TerraformStateLock{}, fmt.Errorf("%w: invalid Terraform lock info", ErrInvalid)
	}
	return lock, nil
}

func lockStateKey(reservationID string) string { return "runner-vm-terraform-state:" + reservationID }

func lockStateRow(ctx context.Context, tx pgx.Tx, reservationID string) error {
	if _, err := tx.Exec(ctx, "SELECT pg_advisory_xact_lock(hashtextextended($1, 31))", lockStateKey(reservationID)); err != nil {
		return fmt.Errorf("%w: lock Terraform state row: %v", ErrUnavailable, err)
	}
	return nil
}

// ReadRunnerVMTerraformState returns the decrypted state only to its separately
// authorized HTTP caller. State is encrypted at rest in PostgreSQL.
func (s *Store) ReadRunnerVMTerraformState(ctx context.Context, reservationID string) ([]byte, bool, error) {
	if s == nil || s.pool == nil || !ValidID(reservationID) {
		return nil, false, ErrInvalid
	}
	if _, err := s.stateAEAD(); err != nil {
		return nil, false, err
	}
	var ciphertext []byte
	var wantHash sql.NullString
	err := s.pool.QueryRow(ctx, `SELECT state_ciphertext,state_sha256 FROM runner_vm_terraform_states WHERE runner_vm_id=$1`, reservationID).
		Scan(&ciphertext, &wantHash)
	if errors.Is(err, pgx.ErrNoRows) {
		return nil, false, nil
	}
	if err != nil {
		return nil, false, fmt.Errorf("%w: read Terraform state: %v", ErrUnavailable, err)
	}
	if len(ciphertext) == 0 {
		return nil, false, nil
	}
	body, err := s.openTerraformState(reservationID, ciphertext)
	if err != nil {
		return nil, false, err
	}
	got := sha256.Sum256(body)
	if !wantHash.Valid || hex.EncodeToString(got[:]) != wantHash.String {
		return nil, false, fmt.Errorf("%w: Terraform state digest mismatch", ErrUnavailable)
	}
	return body, true, nil
}

// RunnerVMTerraformStateLocked reports whether an active Terraform client owns
// the reservation state lock. Callers must not infer apply completion while it is held.
func (s *Store) RunnerVMTerraformStateLocked(ctx context.Context, reservationID string) (bool, error) {
	if s == nil || s.pool == nil || !ValidID(reservationID) {
		return false, ErrInvalid
	}
	var locked bool
	err := s.pool.QueryRow(ctx, `SELECT lock_id IS NOT NULL FROM runner_vm_terraform_states WHERE runner_vm_id=$1`, reservationID).Scan(&locked)
	if errors.Is(err, pgx.ErrNoRows) {
		return false, nil
	}
	if err != nil {
		return false, fmt.Errorf("%w: inspect Terraform state lock: %v", ErrUnavailable, err)
	}
	return locked, nil
}

// LockRunnerVMTerraformState is idempotent for the same Terraform lock ID.
// A different holder receives the durable lock record for a 423 response.
func (s *Store) LockRunnerVMTerraformState(ctx context.Context, actor, reservationID string, raw []byte) ([]byte, bool, error) {
	if s == nil || s.pool == nil || actor == "" || !ValidID(reservationID) {
		return nil, false, ErrInvalid
	}
	lock, err := lockFromJSON(raw)
	if err != nil {
		return nil, false, err
	}
	tx, err := s.pool.Begin(ctx)
	if err != nil {
		return nil, false, fmt.Errorf("%w: begin Terraform state lock: %v", ErrUnavailable, err)
	}
	defer func() { _ = tx.Rollback(ctx) }()
	if err := lockStateRow(ctx, tx, reservationID); err != nil {
		return nil, false, err
	}
	if _, err := tx.Exec(ctx, `INSERT INTO runner_vm_terraform_states (runner_vm_id) VALUES ($1) ON CONFLICT DO NOTHING`, reservationID); err != nil {
		return nil, false, fmt.Errorf("%w: initialize Terraform state lock: %v", ErrConflict, err)
	}
	var currentID *string
	var currentInfo []byte
	if err := tx.QueryRow(ctx, `SELECT lock_id::text,lock_info FROM runner_vm_terraform_states WHERE runner_vm_id=$1 FOR UPDATE`, reservationID).Scan(&currentID, &currentInfo); err != nil {
		return nil, false, fmt.Errorf("%w: read Terraform state lock: %v", ErrUnavailable, err)
	}
	if currentID != nil {
		if *currentID == lock.ID {
			return currentInfo, true, nil
		}
		return currentInfo, false, nil
	}
	_, err = tx.Exec(ctx, `UPDATE runner_vm_terraform_states SET lock_id=$2,lock_info=$3,locked_at=clock_timestamp(),updated_at=clock_timestamp() WHERE runner_vm_id=$1`, reservationID, lock.ID, raw)
	if err != nil {
		return nil, false, fmt.Errorf("%w: persist Terraform state lock: %v", ErrUnavailable, err)
	}
	if err := appendAudit(ctx, tx, actor, "runner_vm.terraform_state.locked", "runner_vm", reservationID, "ok", "unlocked", "locked", "", map[string]any{"lock_id": lock.ID, "operation": lock.Operation}); err != nil {
		return nil, false, fmt.Errorf("%w: audit Terraform state lock: %v", ErrUnavailable, err)
	}
	if err := tx.Commit(ctx); err != nil {
		return nil, false, fmt.Errorf("%w: commit Terraform state lock: %v", ErrUnavailable, err)
	}
	return raw, true, nil
}

// UnlockRunnerVMTerraformState releases only the matching Terraform lock ID.
// A repeated unlock is idempotent; a mismatched lock returns the current info.
func (s *Store) UnlockRunnerVMTerraformState(ctx context.Context, actor, reservationID string, raw []byte) ([]byte, bool, error) {
	if s == nil || s.pool == nil || actor == "" || !ValidID(reservationID) {
		return nil, false, ErrInvalid
	}
	requested, err := lockFromJSON(raw)
	if err != nil {
		return nil, false, err
	}
	tx, err := s.pool.Begin(ctx)
	if err != nil {
		return nil, false, fmt.Errorf("%w: begin Terraform state unlock: %v", ErrUnavailable, err)
	}
	defer func() { _ = tx.Rollback(ctx) }()
	if err := lockStateRow(ctx, tx, reservationID); err != nil {
		return nil, false, err
	}
	var currentID *string
	var currentInfo []byte
	err = tx.QueryRow(ctx, `SELECT lock_id::text,lock_info FROM runner_vm_terraform_states WHERE runner_vm_id=$1 FOR UPDATE`, reservationID).Scan(&currentID, &currentInfo)
	if errors.Is(err, pgx.ErrNoRows) || (err == nil && currentID == nil) {
		return nil, true, nil
	}
	if err != nil {
		return nil, false, fmt.Errorf("%w: read Terraform state lock for release: %v", ErrUnavailable, err)
	}
	if *currentID != requested.ID {
		return currentInfo, false, nil
	}
	_, err = tx.Exec(ctx, `UPDATE runner_vm_terraform_states SET lock_id=NULL,lock_info=NULL,locked_at=NULL,updated_at=clock_timestamp() WHERE runner_vm_id=$1`, reservationID)
	if err != nil {
		return nil, false, fmt.Errorf("%w: release Terraform state lock: %v", ErrUnavailable, err)
	}
	if err := appendAudit(ctx, tx, actor, "runner_vm.terraform_state.unlocked", "runner_vm", reservationID, "ok", "locked", "unlocked", "", map[string]any{"lock_id": requested.ID}); err != nil {
		return nil, false, fmt.Errorf("%w: audit Terraform state unlock: %v", ErrUnavailable, err)
	}
	if err := tx.Commit(ctx); err != nil {
		return nil, false, fmt.Errorf("%w: commit Terraform state unlock: %v", ErrUnavailable, err)
	}
	return nil, true, nil
}

func (s *Store) updateRunnerVMTerraformState(ctx context.Context, actor, reservationID, lockID string, body []byte, remove bool) error {
	if s == nil || s.pool == nil || actor == "" || !ValidID(reservationID) || !ValidID(lockID) {
		return ErrInvalid
	}
	var header terraformStateHeader
	var digest string
	var ciphertext []byte
	var err error
	if !remove {
		header, digest, err = parseTerraformState(body)
		if err != nil {
			return err
		}
		ciphertext, err = s.sealTerraformState(reservationID, body)
		if err != nil {
			return err
		}
	}
	tx, err := s.pool.Begin(ctx)
	if err != nil {
		return fmt.Errorf("%w: begin Terraform state update: %v", ErrUnavailable, err)
	}
	defer func() { _ = tx.Rollback(ctx) }()
	if err := lockStateRow(ctx, tx, reservationID); err != nil {
		return err
	}
	var currentID *string
	var oldLineage *string
	var oldSerial *int64
	var oldHash *string
	err = tx.QueryRow(ctx, `SELECT lock_id::text,lineage,serial,state_sha256 FROM runner_vm_terraform_states WHERE runner_vm_id=$1 FOR UPDATE`, reservationID).
		Scan(&currentID, &oldLineage, &oldSerial, &oldHash)
	if errors.Is(err, pgx.ErrNoRows) || err == nil && currentID == nil {
		return fmt.Errorf("%w: Terraform state update requires an active lock", ErrConflict)
	}
	if err != nil {
		return fmt.Errorf("%w: read Terraform state update fence: %v", ErrUnavailable, err)
	}
	if *currentID != lockID {
		return fmt.Errorf("%w: Terraform state lock ID mismatch", ErrConflict)
	}
	action := "runner_vm.terraform_state.written"
	previous := ""
	next := "present"
	data := map[string]any{"lock_id": lockID}
	if remove {
		if oldHash == nil {
			return tx.Commit(ctx)
		}
		previous = "present"
		action, next = "runner_vm.terraform_state.deleted", "absent"
		data["sha256"] = *oldHash
		_, err = tx.Exec(ctx, `UPDATE runner_vm_terraform_states SET state_ciphertext=NULL,state_sha256=NULL,lineage=NULL,serial=NULL,updated_at=clock_timestamp() WHERE runner_vm_id=$1`, reservationID)
	} else {
		if oldLineage != nil && *oldLineage != header.Lineage {
			return fmt.Errorf("%w: Terraform state lineage changed", ErrConflict)
		}
		if oldSerial != nil {
			if header.Serial < *oldSerial || header.Serial == *oldSerial && *oldHash != digest {
				return fmt.Errorf("%w: Terraform state serial did not advance monotonically", ErrConflict)
			}
			if header.Serial == *oldSerial && *oldHash == digest {
				return tx.Commit(ctx)
			}
		}
		data["serial"], data["lineage"], data["sha256"] = header.Serial, header.Lineage, digest
		_, err = tx.Exec(ctx, `UPDATE runner_vm_terraform_states SET state_ciphertext=$2,state_sha256=$3,lineage=$4,serial=$5,updated_at=clock_timestamp() WHERE runner_vm_id=$1`, reservationID, ciphertext, digest, header.Lineage, header.Serial)
	}
	if err != nil {
		return fmt.Errorf("%w: persist Terraform state: %v", ErrUnavailable, err)
	}
	if err := appendAudit(ctx, tx, actor, action, "runner_vm", reservationID, "ok", previous, next, "", data); err != nil {
		return fmt.Errorf("%w: audit Terraform state update: %v", ErrUnavailable, err)
	}
	if err := tx.Commit(ctx); err != nil {
		return fmt.Errorf("%w: commit Terraform state update: %v", ErrUnavailable, err)
	}
	return nil
}

// WriteRunnerVMTerraformState accepts only a fresh monotonic state snapshot
// from the current backend-lock holder.
func (s *Store) WriteRunnerVMTerraformState(ctx context.Context, actor, reservationID, lockID string, body []byte) error {
	return s.updateRunnerVMTerraformState(ctx, actor, reservationID, lockID, body, false)
}

// DeleteRunnerVMTerraformState purges a state snapshot only under its lock.
func (s *Store) DeleteRunnerVMTerraformState(ctx context.Context, actor, reservationID, lockID string) error {
	return s.updateRunnerVMTerraformState(ctx, actor, reservationID, lockID, nil, true)
}

// LookupRunnerVMRepository binds backend authorization to the reserved job's
// repository rather than to a caller-supplied path or body field.
func (s *Store) LookupRunnerVMRepository(ctx context.Context, reservationID string) (string, error) {
	if s == nil || s.pool == nil || !ValidID(reservationID) {
		return "", ErrInvalid
	}
	var repository string
	err := s.pool.QueryRow(ctx, `SELECT repository FROM runner_vms WHERE id=$1`, reservationID).Scan(&repository)
	if errors.Is(err, pgx.ErrNoRows) {
		return "", ErrNotFound
	}
	if err != nil {
		return "", fmt.Errorf("%w: read runner VM repository: %v", ErrUnavailable, err)
	}
	return repository, nil
}
