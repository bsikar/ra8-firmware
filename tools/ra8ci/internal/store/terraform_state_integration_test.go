//go:build integration

package store

import (
	"bytes"
	"context"
	"crypto/aes"
	"crypto/cipher"
	"encoding/json"
	"errors"
	"testing"
	"time"
)

func TestIntegrationRunnerVMTerraformHTTPStateIsEncryptedLockedAndMonotonic(t *testing.T) {
	s, pool := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()

	block, err := aes.NewCipher(bytes.Repeat([]byte{0x5a}, 32))
	if err != nil {
		t.Fatal(err)
	}
	s.terraformStateAEAD, err = cipher.NewGCM(block)
	if err != nil {
		t.Fatal(err)
	}
	vm, created, err := s.ReserveRunnerVM(ctx, "scaler", runnerVMTestInput(t), testUnclaimedDeadline())
	if err != nil || !created {
		t.Fatalf("reserve state owner: created=%v err=%v", created, err)
	}
	lockID := mustID(t)
	lockBytes, err := json.Marshal(TerraformStateLock{
		ID: lockID, Operation: "OperationTypeApply", Who: "ra8ci@server",
		Version: "1.10.5", Path: "runner/" + vm.ID,
	})
	if err != nil {
		t.Fatal(err)
	}
	_, acquired, err := s.LockRunnerVMTerraformState(ctx, "scaler", vm.ID, lockBytes)
	if err != nil || !acquired {
		t.Fatalf("acquire backend lock: acquired=%v err=%v", acquired, err)
	}
	replayed, acquired, err := s.LockRunnerVMTerraformState(ctx, "scaler", vm.ID, lockBytes)
	var replayedLock TerraformStateLock
	if decodeErr := json.Unmarshal(replayed, &replayedLock); err != nil || decodeErr != nil || !acquired || replayedLock.ID != lockID {
		t.Fatalf("same lock replay was not idempotent: acquired=%v err=%v", acquired, err)
	}
	competing, err := json.Marshal(TerraformStateLock{ID: mustID(t), Operation: "OperationTypeApply", Who: "other"})
	if err != nil {
		t.Fatal(err)
	}
	current, acquired, err := s.LockRunnerVMTerraformState(ctx, "scaler", vm.ID, competing)
	if err != nil || acquired {
		t.Fatalf("competing lock acquired state: acquired=%v err=%v", acquired, err)
	}
	var currentLock TerraformStateLock
	if err := json.Unmarshal(current, &currentLock); err != nil || currentLock.ID != lockID {
		t.Fatalf("lock conflict omitted current holder: %+v err=%v", currentLock, err)
	}

	lineage := mustID(t)
	state := func(serial int, stateLineage string, marker string) []byte {
		body, err := json.Marshal(map[string]any{
			"version": 4, "terraform_version": "1.10.5", "serial": serial, "lineage": stateLineage,
			"outputs":   map[string]any{"fixture": map[string]any{"value": marker, "type": "string"}},
			"resources": []any{},
		})
		if err != nil {
			t.Fatal(err)
		}
		return body
	}
	first := state(0, lineage, "state-secret-marker")
	if err := s.WriteRunnerVMTerraformState(ctx, "scaler", vm.ID, mustID(t), first); !errors.Is(err, ErrConflict) {
		t.Fatalf("state write without current lock was accepted: %v", err)
	}
	if err := s.WriteRunnerVMTerraformState(ctx, "scaler", vm.ID, lockID, first); err != nil {
		t.Fatalf("write first state: %v", err)
	}
	stored, found, err := s.ReadRunnerVMTerraformState(ctx, vm.ID)
	if err != nil || !found || !bytes.Equal(stored, first) {
		t.Fatalf("read state: found=%v err=%v", found, err)
	}
	var ciphertext []byte
	if err := pool.QueryRow(ctx, "SELECT state_ciphertext FROM runner_vm_terraform_states WHERE runner_vm_id=$1", vm.ID).Scan(&ciphertext); err != nil {
		t.Fatal(err)
	}
	if bytes.Contains(ciphertext, []byte("state-secret-marker")) || bytes.Equal(ciphertext, first) {
		t.Fatal("Terraform state was persisted without authenticated encryption")
	}
	if err := s.WriteRunnerVMTerraformState(ctx, "scaler", vm.ID, lockID, state(0, lineage, "changed-at-same-serial")); !errors.Is(err, ErrConflict) {
		t.Fatalf("different state with unchanged serial was accepted: %v", err)
	}
	second := state(1, lineage, "next-state")
	if err := s.WriteRunnerVMTerraformState(ctx, "scaler", vm.ID, lockID, second); err != nil {
		t.Fatalf("write monotonic state: %v", err)
	}
	if err := s.WriteRunnerVMTerraformState(ctx, "scaler", vm.ID, lockID, state(2, mustID(t), "replaced-lineage")); !errors.Is(err, ErrConflict) {
		t.Fatalf("changed Terraform lineage was accepted: %v", err)
	}
	wrongUnlock, released, err := s.UnlockRunnerVMTerraformState(ctx, "scaler", vm.ID, competing)
	if err != nil || released {
		t.Fatalf("mismatched unlock released the current holder: released=%v err=%v", released, err)
	}
	var retainedLock TerraformStateLock
	if err := json.Unmarshal(wrongUnlock, &retainedLock); err != nil || retainedLock.ID != lockID {
		t.Fatalf("mismatched unlock lost current lock info: %+v err=%v", retainedLock, err)
	}
	stored, found, err = s.ReadRunnerVMTerraformState(ctx, vm.ID)
	if err != nil || !found || !bytes.Equal(stored, second) {
		t.Fatalf("monotonic state update: found=%v err=%v", found, err)
	}
	if _, released, err = s.UnlockRunnerVMTerraformState(ctx, "scaler", vm.ID, lockBytes); err != nil || !released {
		t.Fatalf("matching unlock failed: released=%v err=%v", released, err)
	}
	if _, released, err = s.UnlockRunnerVMTerraformState(ctx, "scaler", vm.ID, lockBytes); err != nil || !released {
		t.Fatalf("repeated unlock was not idempotent: released=%v err=%v", released, err)
	}
	if err := s.DeleteRunnerVMTerraformState(ctx, "scaler", vm.ID, lockID); !errors.Is(err, ErrConflict) {
		t.Fatalf("state delete without a lock was accepted: %v", err)
	}
	if _, acquired, err := s.LockRunnerVMTerraformState(ctx, "scaler", vm.ID, lockBytes); err != nil || !acquired {
		t.Fatalf("reacquire lock before delete: acquired=%v err=%v", acquired, err)
	}
	if err := s.DeleteRunnerVMTerraformState(ctx, "scaler", vm.ID, lockID); err != nil {
		t.Fatalf("locked state delete: %v", err)
	}
	if _, found, err := s.ReadRunnerVMTerraformState(ctx, vm.ID); err != nil || found {
		t.Fatalf("deleted state remained readable: found=%v err=%v", found, err)
	}
	var auditCount int
	if err := pool.QueryRow(ctx, "SELECT count(*) FROM audit WHERE target_type='runner_vm' AND target_id=$1 AND action LIKE 'runner_vm.terraform_state.%'", vm.ID).Scan(&auditCount); err != nil || auditCount < 5 {
		t.Fatalf("Terraform state audit trail incomplete: count=%d err=%v", auditCount, err)
	}
}
