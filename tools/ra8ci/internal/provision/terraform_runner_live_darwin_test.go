//go:build darwin

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package provision

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"net"
	"net/netip"
	"os"
	"path/filepath"
	goruntime "runtime"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/privatefile"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/proxmox"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// liveRunnerConfig contains only endpoints and protected-file paths. The
// wrapper reads OpenBao and state-encryption secrets from the controller's
// protected store; this file never contains credential values.
type liveRunnerConfig struct {
	Terraform TerraformConfig
	Proxmox   struct {
		Endpoint  string
		CAFile    string
		TokenFile string
	}
	Runner struct {
		Actor             string
		OpenBaoAddress    string
		OpenBaoKVMount    string
		OpenBaoSecretPath string
		ModuleDirectory   string
	}
}

type liveRunnerLedger struct {
	vm      store.RunnerVM
	ops     map[string]store.RunnerVMOperation
	state   []byte
	opIndex int
}

func (l *liveRunnerLedger) GetRunnerVM(_ context.Context, id string) (store.RunnerVM, error) {
	if l.vm.ID == "" || l.vm.ID != id {
		return store.RunnerVM{}, store.ErrNotFound
	}
	return l.vm, nil
}

func (l *liveRunnerLedger) GetRunnerVMByJob(_ context.Context, scaleSetID int64, jobID string) (store.RunnerVM, error) {
	if l.vm.ID == "" || l.vm.ScaleSetID != scaleSetID || l.vm.JobID != jobID {
		return store.RunnerVM{}, store.ErrNotFound
	}
	return l.vm, nil
}

func (l *liveRunnerLedger) ReserveRunnerVM(_ context.Context, _ string, input store.RunnerVMInput,
	deadline time.Time) (store.RunnerVM, bool, error) {
	if l.vm.ID != "" || !deadline.After(time.Now()) {
		return store.RunnerVM{}, false, store.ErrConflict
	}
	l.vm = store.RunnerVM{ID: "0192f3a4-b5c6-7d8e-9f01-1234567890ab",
		RunnerVMInput: input, CreationOperationID: "0192f3a4-b5c6-7d8e-9f01-1234567890ac",
		State: "reserved", Generation: 1}
	return l.vm, true, nil
}

func (l *liveRunnerLedger) ListActiveRunnerVMIDs(context.Context, string) ([]int, error) {
	if l.vm.ID != "" && l.vm.State != "released" {
		return []int{l.vm.VMID}, nil
	}
	return nil, nil
}

func (l *liveRunnerLedger) GetRunnerVMOperation(_ context.Context, id string) (store.RunnerVMOperation, error) {
	op, ok := l.ops[id]
	if !ok {
		return store.RunnerVMOperation{}, store.ErrNotFound
	}
	return op, nil
}

func (l *liveRunnerLedger) RecordRunnerVMTerraformPlan(_ context.Context, _, _ string, _ int64,
	id string, evidence store.RunnerVMTerraformPlanEvidence) error {
	op, ok := l.ops[id]
	if !ok || op.Status != "unresolved" {
		return store.ErrConflict
	}
	op.ProviderKind = "terraform"
	op.TerraformVersion = evidence.TerraformVersion
	op.PlanSHA256 = evidence.PlanSHA256
	op.ModuleSHA256 = evidence.ModuleSHA256
	op.InputSHA256 = evidence.InputSHA256
	op.ProviderLockSHA256 = evidence.ProviderLockSHA256
	op.StateIdentitySHA256 = evidence.StateIdentitySHA256
	l.ops[id] = op
	return nil
}

func (l *liveRunnerLedger) BeginRunnerVMTerraformApply(_ context.Context, _, _ string, _ int64,
	id, digest string) (bool, error) {
	op, ok := l.ops[id]
	if !ok || op.ProviderKind != "terraform" || op.PlanSHA256 != digest || op.TerraformApplyStartedAt != nil {
		return false, store.ErrConflict
	}
	now := time.Now().UTC()
	op.TerraformApplyStartedAt = &now
	l.ops[id] = op
	l.vm.CurrentOperationID = id
	l.vm.UnknownOutcome = true
	return true, nil
}

func (l *liveRunnerLedger) ReadRunnerVMTerraformState(context.Context, string) ([]byte, bool, error) {
	return append([]byte(nil), l.state...), len(l.state) != 0, nil
}

func (*liveRunnerLedger) RunnerVMTerraformStateLocked(context.Context, string) (bool, error) {
	return false, nil
}

func (l *liveRunnerLedger) begin(kind string) proxmox.Action {
	id := fmt.Sprintf("0192f3a4-b5c6-7d8e-9f01-%012x", 0x1000+l.opIndex)
	l.opIndex++
	l.ops[id] = store.RunnerVMOperation{ID: id, RunnerVMID: l.vm.ID,
		Kind: kind, FromState: l.vm.State, PendingState: l.vm.State,
		Status: "unresolved", Generation: l.vm.Generation + 1, ProviderKind: "proxmox"}
	l.vm.CurrentOperationID = id
	l.vm.UnknownOutcome = true
	return proxmox.Action{ID: id}
}

func (l *liveRunnerLedger) settle(action proxmox.Action, result proxmox.Result) error {
	op, ok := l.ops[action.ID]
	if !ok || result.TerraformEvidence == nil ||
		(result.TerraformEvidence.Outcome != "succeeded" && result.TerraformEvidence.Outcome != "failed") {
		return errors.New("lifecycle operation did not reconcile to success")
	}
	op.Status = result.TerraformEvidence.Outcome
	op.ReconciliationSHA256 = result.TerraformEvidence.ReconciliationSHA256
	l.ops[action.ID] = op
	l.vm.CurrentOperationID = ""
	l.vm.UnknownOutcome = false
	l.vm.Generation++
	if result.TerraformEvidence.Outcome == "failed" {
		l.vm.State = op.FromState
		return nil
	}
	switch op.Kind {
	case "clone", "stop":
		l.vm.State = "stopped"
	case "start":
		l.vm.State = "running"
	case "destroy":
		l.vm.State = "released"
	default:
		return errors.New("unknown lifecycle operation")
	}
	return nil
}

func (l *liveRunnerLedger) settleNoEffect(action proxmox.Action) error {
	op, ok := l.ops[action.ID]
	if !ok || op.Status != "unresolved" || op.TerraformApplyStartedAt != nil {
		return store.ErrConflict
	}
	op.Status = "failed"
	l.ops[action.ID] = op
	l.vm.State = op.FromState
	l.vm.CurrentOperationID = ""
	l.vm.UnknownOutcome = false
	l.vm.Generation++
	return nil
}

type liveRunnerRuntime struct {
	inner  *TerraformRuntime
	ledger *liveRunnerLedger
}

func (r *liveRunnerRuntime) runnerConfig() TerraformConfig { return r.inner.runnerConfig() }

func (r *liveRunnerRuntime) WithSession(ctx context.Context, reservationID string,
	callback func(*TerraformSession) error) error {
	return r.inner.WithSession(ctx, reservationID, func(session *TerraformSession) error {
		if err := callback(session); err != nil {
			return err
		}
		state, err := session.StatePull(ctx)
		if err != nil {
			return err
		}
		r.ledger.state = append(r.ledger.state[:0], state...)
		clear(state)
		return nil
	})
}

func TestLiveTerraformRunnerLifecycle(t *testing.T) {
	if os.Getenv("RA8CI_LAB_LIFECYCLE") != "1" {
		t.Skip("set RA8CI_LAB_LIFECYCLE=1 on the controller to run live lifecycle acceptance")
	}
	configPath := os.Getenv("RA8CI_LAB_LIFECYCLE_CONFIG")
	if !filepath.IsAbs(configPath) || privatefile.Check(configPath) != nil {
		t.Fatal("live lifecycle config must be an absolute owner-only file")
	}
	body, err := os.ReadFile(configPath)
	if err != nil {
		t.Fatal("read protected live lifecycle config")
	}
	var config liveRunnerConfig
	decodeErr := json.Unmarshal(body, &config)
	clear(body)
	if decodeErr != nil {
		t.Fatal("decode protected live lifecycle config")
	}
	_, sourceFile, _, sourceOK := goruntime.Caller(0)
	if !sourceOK {
		t.Fatal("locate live lifecycle test source")
	}
	repositoryRoot := filepath.Clean(filepath.Join(filepath.Dir(sourceFile), "../../../.."))
	wrapperPath := filepath.Join(repositoryRoot, "infra", "terraform", "run-with-openbao.sh")
	if config.Terraform.CommandWrapper != wrapperPath {
		t.Fatal("live lifecycle requires the repository OpenBao wrapper")
	}
	wrapperRoot := filepath.Dir(wrapperPath)
	if config.Terraform.EnvironmentDirectory != filepath.Join(wrapperRoot, "environments", "ra8ci-runner") ||
		config.Runner.ModuleDirectory != filepath.Join(wrapperRoot, "modules", "ra8ci_ephemeral_runner") {
		t.Fatal("live lifecycle must use the current ra8ci-runner environment and module")
	}
	operatorHome, err := os.UserHomeDir()
	if err != nil || config.Terraform.WrapperHome != operatorHome {
		t.Fatal("live lifecycle wrapper must use the current controller user's protected Keychain context")
	}

	ctx, cancel := context.WithTimeout(context.Background(), 90*time.Minute)
	defer cancel()
	runtime, err := OpenPinnedTerraformRuntime(ctx, config.Terraform)
	if err != nil {
		t.Fatal("open pinned OpenTofu runtime through the OpenBao wrapper")
	}
	allowedIDs := make([]int, 0, 19)
	for vmid := 9020; vmid <= 9039; vmid++ {
		if vmid != windowsLifecycleVMID {
			allowedIDs = append(allowedIDs, vmid)
		}
	}
	client, err := proxmox.New(proxmox.Config{Endpoint: config.Proxmox.Endpoint,
		CAFile: config.Proxmox.CAFile, TokenFile: config.Proxmox.TokenFile,
		Node: "pve1", Pool: "ra8-tf-lab", Storage: "ra8-tf-lab",
		AllowedVMIDs: allowedIDs, TemplateVMIDs: []int{9001}, Bridges: []string{"vmbr9"},
		OperationTimeout: 10 * time.Minute})
	if err != nil {
		t.Fatal("open constrained Proxmox client")
	}

	ledger := &liveRunnerLedger{ops: make(map[string]store.RunnerVMOperation)}
	profiles := make(map[int]TerraformRunnerProfile, 19)
	for vmid := 9020; vmid <= 9039; vmid++ {
		if vmid == windowsLifecycleVMID {
			continue
		}
		profiles[vmid] = TerraformRunnerProfile{
			TemplateVMID: 9001, TemplateName: "ra8-lab-debian-template",
			Node: "pve1", Pool: "ra8-tf-lab", DatastoreID: "ra8-tf-lab", Bridge: "vmbr9",
			Cores: 4, MemoryMB: 8192, IPv4Address: fmt.Sprintf("10.250.9.%d/24", vmid-8970),
			IPv4Gateway: "10.250.9.1", UserName: "ra8ci",
		}
	}
	keys, err := NewSSHAccessStore(filepath.Join(config.Terraform.StateDirectory, "live-ssh-keys"))
	if err != nil {
		t.Fatal("prepare private runner key store")
	}
	provisioner, err := NewTerraformRunnerProvisioner(&liveRunnerRuntime{inner: runtime, ledger: ledger},
		ledger, client, keys, TerraformRunnerConfig{Actor: config.Runner.Actor,
			ProxmoxEndpoint: config.Proxmox.Endpoint, OpenBaoAddress: config.Runner.OpenBaoAddress,
			OpenBaoKVMount: config.Runner.OpenBaoKVMount, OpenBaoSecretPath: config.Runner.OpenBaoSecretPath,
			Profiles: profiles, ModuleDirectory: config.Runner.ModuleDirectory})
	if err != nil {
		t.Fatal("construct constrained Terraform runner provisioner")
	}
	input := store.RunnerVMInput{ScaleSetID: 1, JobID: "live-lifecycle-acceptance",
		RunnerRequestID: 1, WorkflowRunID: time.Now().Unix(), WorkflowAttempt: 1,
		Repository: "owner/repo", WorkflowRef: "refs/heads/dev",
		CommitSHA: "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}
	vm, created, err := provisioner.Reserve(ctx, input, time.Now().Add(time.Hour))
	if err != nil || !created {
		t.Fatal("reserve live runner VMID")
	}
	identity := proxmox.Identity{VMID: vm.VMID, Node: vm.Node, Pool: vm.Pool, Storage: vm.Storage,
		Name: vm.Name, ReservationID: vm.ID, CreationOperationID: vm.CreationOperationID,
		RunID: fmt.Sprintf("%016x", uint64(vm.WorkflowRunID))}
	t.Cleanup(func() {
		if err := cleanupLiveRunner(provisioner, client, ledger, identity); err != nil {
			t.Errorf("live lifecycle cleanup needs operator attention: %v", err)
		}
	})
	profile := profiles[vm.VMID]

	clone := ledger.begin("clone")
	cloneResult, err := provisioner.Clone(ctx, clone, proxmox.CloneSpec{Target: identity,
		TemplateVMID: 9001, TemplateName: vm.TemplateName, TemplateDigest: vm.TemplateDigest})
	if err != nil || ledger.settle(clone, cloneResult) != nil {
		t.Fatal("clone and reconcile runner guest")
	}
	start := ledger.begin("start")
	startResult, err := provisioner.Start(ctx, start, identity)
	if err != nil || ledger.settle(start, startResult) != nil {
		t.Fatal("start and reconcile runner guest")
	}
	probeRunnerSSH(ctx, t, profile.IPv4Address)
	idle := proxmox.IdleProof{VMID: vm.VMID, ReservationID: vm.ID,
		EvidenceID: "0192f3a4-b5c6-7d8e-9f01-1234567890b1", ObservedAt: time.Now(),
		Drained: true, NoActiveJob: true}
	stop := ledger.begin("stop")
	stopResult, err := provisioner.Stop(ctx, stop, identity, idle)
	if err != nil || ledger.settle(stop, stopResult) != nil {
		t.Fatal("stop and reconcile runner guest")
	}
	observed, err := client.Get(ctx, identity)
	if err != nil {
		t.Fatal("observe stopped runner guest before destroy")
	}
	destroy := ledger.begin("destroy")
	destroyResult, err := provisioner.Destroy(ctx, destroy, identity, proxmox.DestroyProof{
		IdleProof: liveIdleProof(identity), ApprovalID: "0192f3a4-b5c6-7d8e-9f01-1234567890b2",
		ExpectedConfigDigest: observed.ConfigDigest, RunnerDeregistered: true, StateReconciled: true,
	})
	if err != nil || ledger.settle(destroy, destroyResult) != nil {
		t.Fatal("destroy and reconcile runner guest")
	}
	if _, err := client.Get(ctx, identity); !errors.Is(err, proxmox.ErrNotFound) {
		t.Fatal("independent Proxmox read found the runner VMID after destroy")
	}
	occupied, err := client.OccupiedVMIDs(ctx)
	if err != nil {
		t.Fatal("independently list Proxmox VMIDs after destroy")
	}
	for _, vmid := range occupied {
		if vmid == identity.VMID {
			t.Fatal("independent Proxmox inventory still contains the runner VMID")
		}
	}
	t.Logf("live lifecycle passed: VMID %d cloned, started, boot-probed, stopped, destroyed, reconciled, and absent", identity.VMID)
}

func probeRunnerSSH(ctx context.Context, t *testing.T, prefix string) {
	t.Helper()
	network, err := netip.ParsePrefix(prefix)
	if err != nil {
		t.Fatal("invalid reviewed runner SSH address")
	}
	address := net.JoinHostPort(network.Addr().String(), "22")
	deadline := time.Now().Add(4 * time.Minute)
	for time.Now().Before(deadline) {
		probeCtx, cancel := context.WithTimeout(ctx, 3*time.Second)
		connection, dialErr := (&net.Dialer{}).DialContext(probeCtx, "tcp", address)
		cancel()
		if dialErr == nil {
			_ = connection.Close()
			return
		}
		time.Sleep(2 * time.Second)
	}
	t.Fatal("runner did not pass the bounded SSH boot probe")
}

func cleanupLiveRunner(provisioner *TerraformRunnerProvisioner, client *proxmox.Client,
	ledger *liveRunnerLedger, identity proxmox.Identity) error {
	ctx, cancel := context.WithTimeout(context.Background(), 40*time.Minute)
	defer cancel()
	observed, err := client.Get(ctx, identity)
	if errors.Is(err, proxmox.ErrNotFound) {
		return nil
	}
	if err != nil {
		return errors.New("cannot independently observe the runner guest")
	}
	if observed.Locked || observed.Protected {
		return errors.New("runner guest is locked or protected; cleanup refused")
	}
	if ledger.vm.UnknownOutcome && ledger.vm.CurrentOperationID != "" {
		op, getErr := ledger.GetRunnerVMOperation(ctx, ledger.vm.CurrentOperationID)
		if getErr != nil {
			return errors.New("cannot read unresolved lifecycle operation")
		}
		result, reconcileErr := provisioner.Reconcile(ctx, op.ID, identity, op.Kind, "")
		if reconcileErr != nil {
			return errors.New("cannot reconcile unresolved lifecycle operation")
		}
		if result.TerraformEvidence != nil {
			if ledger.settle(proxmox.Action{ID: op.ID}, result) != nil {
				return errors.New("cannot settle reconciled lifecycle operation")
			}
		} else if result.TerraformPreflightNoEffect != nil {
			if ledger.settleNoEffect(proxmox.Action{ID: op.ID}) != nil {
				return errors.New("cannot settle lifecycle operation with no effect")
			}
		} else {
			return errors.New("lifecycle operation has no safe reconciliation evidence")
		}
		observed, err = client.Get(ctx, identity)
		if errors.Is(err, proxmox.ErrNotFound) {
			return nil
		}
		if err != nil || observed.Locked || observed.Protected {
			return errors.New("runner guest remains unavailable, locked, or protected after reconciliation")
		}
	}
	if observed.Status == "running" {
		stop := ledger.begin("stop")
		result, stopErr := provisioner.Stop(ctx, stop, identity, liveIdleProof(identity))
		if stopErr != nil || ledger.settle(stop, result) != nil {
			return errors.New("could not safely stop runner guest during cleanup")
		}
		observed, err = client.Get(ctx, identity)
		if err != nil {
			return errors.New("could not verify stopped runner guest during cleanup")
		}
	}
	if observed.Status != "stopped" {
		return errors.New("runner guest is not stopped; cleanup refused")
	}
	destroy := ledger.begin("destroy")
	result, destroyErr := provisioner.Destroy(ctx, destroy, identity, proxmox.DestroyProof{
		IdleProof: liveIdleProof(identity), ApprovalID: "0192f3a4-b5c6-7d8e-9f01-1234567890b3",
		ExpectedConfigDigest: observed.ConfigDigest, RunnerDeregistered: true, StateReconciled: true,
	})
	if destroyErr != nil || ledger.settle(destroy, result) != nil {
		return errors.New("could not destroy runner guest during cleanup")
	}
	if _, err := client.Get(ctx, identity); !errors.Is(err, proxmox.ErrNotFound) {
		return errors.New("independent Proxmox read still finds runner guest after cleanup")
	}
	return nil
}

func liveIdleProof(identity proxmox.Identity) proxmox.IdleProof {
	return proxmox.IdleProof{VMID: identity.VMID, ReservationID: identity.ReservationID,
		EvidenceID: "0192f3a4-b5c6-7d8e-9f01-1234567890b4", ObservedAt: time.Now(),
		Drained: true, NoActiveJob: true}
}
