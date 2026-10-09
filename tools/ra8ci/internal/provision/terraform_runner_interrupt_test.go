// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package provision

import (
	"context"
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/proxmox"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

var errApplyInterrupted = errors.New("simulated controller interruption after apply dispatch")

type interruptedRunnerRuntime struct {
	t         *testing.T
	recorder  *commandRecorder
	interrupt bool
}

func (r *interruptedRunnerRuntime) runnerConfig() TerraformConfig {
	return r.recorder.config
}

func (r *interruptedRunnerRuntime) WithSession(ctx context.Context, reservationID string,
	callback func(*TerraformSession) error) error {
	workspace := filepath.Join(r.recorder.config.StateDirectory, reservationID)
	if err := secureDirectory(workspace); err != nil {
		return err
	}
	session := &TerraformSession{runtime: r.recorder.runtime, reservationID: reservationID,
		workspace: workspace, environment: []string{"TF_IN_AUTOMATION=1",
			"TF_DATA_DIR=" + filepath.Join(workspace, "tfdata")}}
	if err := callback(session); err != nil {
		return err
	}
	if r.interrupt {
		return errApplyInterrupted
	}
	return nil
}

type interruptedRunnerLedger struct {
	policyLedger
	vm    store.RunnerVM
	op    store.RunnerVMOperation
	state []byte
}

func (l *interruptedRunnerLedger) GetRunnerVM(_ context.Context, id string) (store.RunnerVM, error) {
	if id != l.vm.ID {
		return store.RunnerVM{}, store.ErrNotFound
	}
	return l.vm, nil
}

func (l *interruptedRunnerLedger) GetRunnerVMOperation(_ context.Context, id string) (store.RunnerVMOperation, error) {
	if id != l.op.ID {
		return store.RunnerVMOperation{}, store.ErrNotFound
	}
	return l.op, nil
}

func (l *interruptedRunnerLedger) RecordRunnerVMTerraformPlan(_ context.Context, _, _ string, _ int64,
	operationID string, evidence store.RunnerVMTerraformPlanEvidence) error {
	if operationID != l.op.ID {
		return store.ErrConflict
	}
	l.op.ProviderKind = "terraform"
	l.op.TerraformVersion = evidence.TerraformVersion
	l.op.PlanSHA256 = evidence.PlanSHA256
	l.op.ModuleSHA256 = evidence.ModuleSHA256
	l.op.InputSHA256 = evidence.InputSHA256
	l.op.ProviderLockSHA256 = evidence.ProviderLockSHA256
	l.op.StateIdentitySHA256 = evidence.StateIdentitySHA256
	return nil
}

func (l *interruptedRunnerLedger) BeginRunnerVMTerraformApply(_ context.Context, _, _ string, _ int64,
	operationID, digest string) (bool, error) {
	if operationID != l.op.ID || digest != l.op.PlanSHA256 || l.op.TerraformApplyStartedAt != nil {
		return false, store.ErrConflict
	}
	now := time.Now().UTC()
	l.op.TerraformApplyStartedAt = &now
	return true, nil
}

func (l *interruptedRunnerLedger) ReadRunnerVMTerraformState(context.Context, string) ([]byte, bool, error) {
	return append([]byte(nil), l.state...), true, nil
}

func (l *interruptedRunnerLedger) RunnerVMTerraformStateLocked(context.Context, string) (bool, error) {
	return false, nil
}

func (l *interruptedRunnerLedger) settleObserved(operationID string, evidence *proxmox.TerraformEvidence) error {
	if evidence == nil || operationID != l.op.ID || l.op.Status != "unresolved" ||
		evidence.Outcome != "succeeded" || !evidence.StateHasVM || evidence.VMAbsent || evidence.VMStatus != "stopped" {
		return store.ErrConflict
	}
	l.op.Status = "succeeded"
	l.vm.State = "stopped"
	l.vm.UnknownOutcome = false
	l.vm.CurrentOperationID = ""
	return nil
}

type interruptedRunnerObserver struct {
	policyObserver
	gets int
}

func (o *interruptedRunnerObserver) Get(_ context.Context, identity proxmox.Identity) (proxmox.VM, error) {
	o.gets++
	if o.gets == 1 {
		return proxmox.VM{}, proxmox.ErrNotFound
	}
	return proxmox.VM{Identity: identity, Status: "stopped", ConfigDigest: "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}, nil
}

func (o *interruptedRunnerObserver) GetTemplate(context.Context, int) (proxmox.Template, error) {
	return proxmox.Template{VMID: 9001, Node: "pve1", Pool: "ra8-tf-lab",
		Name: "ra8-lab-debian-template", Status: "stopped", Template: true,
		ConfigDigest: "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}, nil
}

func (*interruptedRunnerObserver) BridgePresent(context.Context, string) (bool, error) {
	return true, nil
}

func (*interruptedRunnerObserver) OccupiedVMIDs(context.Context) ([]int, error) { return nil, nil }

func TestInterruptedTerraformCloneReconcilesToObservedStoppedGuest(t *testing.T) {
	recorder := terraformStandIn(t, "exit 0")
	runnerEnvironment := filepath.Join(t.TempDir(), "ra8ci-runner")
	if err := os.MkdirAll(runnerEnvironment, 0o700); err != nil {
		t.Fatal(err)
	}
	recorder.config.EnvironmentDirectory = runnerEnvironment
	recorder.runtime.config.EnvironmentDirectory = runnerEnvironment
	lockfile := filepath.Join(recorder.config.EnvironmentDirectory, ".terraform.lock.hcl")
	if err := os.WriteFile(lockfile, []byte("provider lock fixture"), 0o600); err != nil {
		t.Fatal(err)
	}
	moduleDirectory := t.TempDir()
	if err := os.WriteFile(filepath.Join(moduleDirectory, "main.tf"), []byte("module fixture"), 0o600); err != nil {
		t.Fatal(err)
	}
	identity := reservedIdentity()
	vm := reservedVM(identity)
	vm.WorkflowRunID = 12345
	identity.RunID = "0000000000003039"
	vm.TemplateName = "ra8-lab-debian-template"
	vm.TemplateDigest = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
	vm.CurrentOperationID = identity.CreationOperationID
	vm.UnknownOutcome = true
	ledger := &interruptedRunnerLedger{
		vm: vm,
		op: store.RunnerVMOperation{ID: identity.CreationOperationID, RunnerVMID: identity.ReservationID,
			Kind: "clone", Status: "unresolved", Generation: 1, ProviderKind: "proxmox"},
		state: terraformTestState(vm),
	}
	observer := &interruptedRunnerObserver{}
	keys := mustKeys(t)
	config := reviewedConfig(t)
	config.ModuleDirectory = moduleDirectory
	runtime := &interruptedRunnerRuntime{t: t, recorder: recorder, interrupt: true}
	provisioner, err := NewTerraformRunnerProvisioner(runtime, ledger, observer, keys, config)
	if err != nil {
		t.Fatal(err)
	}
	_, err = provisioner.Clone(context.Background(), proxmox.Action{ID: identity.CreationOperationID},
		proxmox.CloneSpec{Target: identity, TemplateVMID: 9001,
			TemplateName: "ra8-lab-debian-template", TemplateDigest: "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"})
	if !errors.Is(err, proxmox.ErrUnknownOutcome) {
		t.Fatalf("interrupted apply must be reported as unknown until reconciliation, got %v", err)
	}
	inputPath := filepath.Join(recorder.config.StateDirectory, identity.ReservationID,
		identity.CreationOperationID, "runner.tfvars.json")
	inputBody, err := os.ReadFile(inputPath)
	if err != nil {
		t.Fatalf("read private runner input fixture: %v", err)
	}
	var runnerInput map[string]any
	decodeErr := json.Unmarshal(inputBody, &runnerInput)
	clear(inputBody)
	if decodeErr != nil || runnerInput["runner_enabled"] != true {
		t.Fatalf("runner root must be enabled for lifecycle apply: enabled=%v err=%v",
			runnerInput["runner_enabled"], decodeErr)
	}
	if ledger.op.TerraformApplyStartedAt == nil || ledger.op.PlanSHA256 == "" || ledger.op.ModuleSHA256 == "" ||
		ledger.op.ProviderLockSHA256 == "" || ledger.op.ID != identity.CreationOperationID {
		t.Fatalf("apply intent did not retain operation and plan/module/lock evidence: %+v", ledger.op)
	}
	result, err := provisioner.Reconcile(context.Background(), identity.CreationOperationID, identity, "clone", "")
	if err != nil || result.TerraformEvidence == nil || result.TerraformEvidence.Outcome != "succeeded" ||
		result.TerraformEvidence.VMStatus != "stopped" || !result.TerraformEvidence.StateHasVM {
		t.Fatalf("reconcile did not settle to the observed stopped guest: result=%+v err=%v", result, err)
	}
	if err := ledger.settleObserved(identity.CreationOperationID, result.TerraformEvidence); err != nil ||
		ledger.vm.State != "stopped" || ledger.vm.UnknownOutcome || ledger.op.Status != "succeeded" {
		t.Fatalf("ledger did not record the reconciled stopped state: vm=%+v op=%+v err=%v", ledger.vm, ledger.op, err)
	}
}
