// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package scaler

import (
	"context"
	"encoding/json"
	"encoding/pem"
	"errors"
	"fmt"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/actions/scaleset"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/github"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/proxmox"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

const (
	testSHA      = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
	testDigest   = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
	testWorkflow = "bsikar/ra8-firmware/.github/workflows/ci.yml@refs/heads/dev"
)

type memoryLedger struct {
	mu           sync.Mutex
	vm           store.RunnerVM
	op           store.RunnerVMOperation
	bootstrap    []store.RunnerVMBootstrapEvidence
	loseUPIDOnce bool
	reserveCalls int
	beginCalls   int

	resolveCalls int

	resolvedProof store.RunnerVMResolution
}

func (m *memoryLedger) RecordRunnerVMBootstrapEvidence(_ context.Context, _ string, evidence store.RunnerVMBootstrapEvidence) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	if m.vm.ID != evidence.ReservationID || m.vm.VMID != evidence.VMID ||
		m.vm.CommitSHA != evidence.CommitSHA || m.vm.State != "running" ||
		m.vm.CleanupRequested || m.vm.UnknownOutcome {
		return store.ErrConflict
	}
	m.bootstrap = append(m.bootstrap, evidence)
	return nil
}

func (m *memoryLedger) GetRunnerVMByJob(_ context.Context, scaleSetID int64, jobID string) (store.RunnerVM, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	if m.vm.ID == "" || m.vm.ScaleSetID != scaleSetID || m.vm.JobID != jobID {
		return store.RunnerVM{}, store.ErrNotFound
	}
	return m.vm, nil
}

func (m *memoryLedger) GetRunnerVM(_ context.Context, id string) (store.RunnerVM, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	if m.vm.ID != id {
		return store.RunnerVM{}, store.ErrNotFound
	}
	return m.vm, nil
}

func (m *memoryLedger) GetRunnerVMOperation(_ context.Context, id string) (store.RunnerVMOperation, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	if m.op.ID != id {
		return store.RunnerVMOperation{}, store.ErrNotFound
	}
	return m.op, nil
}

func (m *memoryLedger) ReserveRunnerVM(_ context.Context, _ string, input store.RunnerVMInput) (store.RunnerVM, bool, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	m.reserveCalls++
	if m.vm.ID != "" {
		if m.vm.RunnerVMInput == input {
			return m.vm, false, nil
		}
		return store.RunnerVM{}, false, store.ErrConflict
	}
	id, err := store.NewID()
	if err != nil {
		return store.RunnerVM{}, false, err
	}
	creation, err := store.NewID()
	if err != nil {
		return store.RunnerVM{}, false, err
	}
	m.vm = store.RunnerVM{ID: id, RunnerVMInput: input, CreationOperationID: creation, State: "reserved", Generation: 1}
	return m.vm, true, nil
}

func (m *memoryLedger) BeginRunnerVMOperation(_ context.Context, _, id string, generation int64, kind string, proof store.RunnerVMSafetyEvidence) (store.RunnerVMOperation, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	m.beginCalls++
	if m.vm.ID != id {
		return store.RunnerVMOperation{}, store.ErrNotFound
	}
	if m.vm.UnknownOutcome {
		if m.op.Kind != kind {
			return store.RunnerVMOperation{}, store.ErrConflict
		}
		prior := m.op
		prior.PriorRequestIssued = true
		return prior, nil
	}
	if m.vm.Generation != generation {
		return store.RunnerVMOperation{}, store.ErrConflict
	}
	from := m.vm.State
	pending := map[string]string{"clone": "cloning", "start": "starting", "stop": "stopping", "destroy": "deleting"}[kind]
	if pending == "" || (kind == "clone" && from != "reserved") || (kind == "start" && (from != "stopped" || m.vm.CleanupRequested)) || (kind == "stop" && from != "draining") || (kind == "destroy" && from != "stopped") {
		return store.RunnerVMOperation{}, store.ErrConflict
	}
	if kind == "stop" || kind == "destroy" {
		if !proof.Drained || !proof.NoActiveJob || !fresh(proof.ObservedAt, 10*time.Second) {
			return store.RunnerVMOperation{}, store.ErrDenied
		}
	}
	opID := m.vm.CreationOperationID
	if kind != "clone" {
		var err error
		opID, err = store.NewID()
		if err != nil {
			return store.RunnerVMOperation{}, err
		}
	}
	m.vm.State = pending
	m.vm.Generation++
	m.vm.UnknownOutcome = true
	m.vm.CurrentOperationID = opID
	m.op = store.RunnerVMOperation{ID: opID, RunnerVMID: id, Kind: kind, FromState: from, PendingState: pending, Generation: m.vm.Generation, Status: "unresolved"}
	return m.op, nil
}

func (m *memoryLedger) RecordRunnerVMUPID(_ context.Context, _, id string, generation int64, opID, upid string) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	if m.loseUPIDOnce {
		m.loseUPIDOnce = false
		return store.ErrUnavailable
	}
	if m.vm.ID != id || m.vm.Generation != generation || m.op.ID != opID || !m.vm.UnknownOutcome {
		return store.ErrConflict
	}
	if m.op.UPID != "" && m.op.UPID != upid {
		return store.ErrConflict
	}
	m.op.UPID = upid
	return nil
}

func (m *memoryLedger) ResolveRunnerVMOperation(_ context.Context, _, id string, generation int64, opID string, proof store.RunnerVMResolution) (store.RunnerVM, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	if m.vm.ID != id || m.vm.Generation != generation || m.op.ID != opID || !m.vm.UnknownOutcome || !proof.PostStateVerified {
		return store.RunnerVM{}, store.ErrConflict
	}
	if proof.Source == "terraform_preflight" {
		if proof.Outcome != "failed" || (m.op.ProviderKind != "proxmox" && m.op.ProviderKind != "terraform") ||
			m.op.UPID != "" || m.op.TerraformApplyStartedAt != nil || proof.ReconciliationSHA256 != "" {
			return store.RunnerVM{}, store.ErrDenied
		}
		if m.op.ProviderKind == "proxmox" && (m.op.PlanSHA256 != "" || proof.PlanSHA256 != "" || proof.StateIdentitySHA256 != "") {
			return store.RunnerVM{}, store.ErrDenied
		}
		if m.op.ProviderKind == "terraform" && (m.op.PlanSHA256 == "" || proof.PlanSHA256 != m.op.PlanSHA256 || proof.StateIdentitySHA256 != m.op.StateIdentitySHA256) {
			return store.RunnerVM{}, store.ErrDenied
		}
		m.resolveCalls++
		m.vm.State = m.op.FromState
		m.vm.Generation++
		m.vm.UnknownOutcome = false
		m.vm.CurrentOperationID = ""
		m.op.Status = "failed"
		return m.vm, nil
	}
	if proof.Outcome != "succeeded" {
		return store.RunnerVM{}, store.ErrConflict
	}
	if proof.Source == "upid" && m.op.UPID == "" {
		return store.RunnerVM{}, store.ErrDenied
	}
	if proof.Source == "clone_marker" && m.op.Kind != "clone" {
		return store.RunnerVM{}, store.ErrDenied
	}
	if proof.Source == "terraform_state" && (m.op.ProviderKind != "terraform" ||
		proof.PlanSHA256 != m.op.PlanSHA256 || proof.StateIdentitySHA256 != m.op.StateIdentitySHA256) {
		return store.RunnerVM{}, store.ErrDenied
	}
	m.resolvedProof = proof
	m.resolveCalls++
	m.vm.State = store.VMOperationSuccessState(m.op.Kind)
	if m.op.Kind == "start" && m.vm.CleanupRequested {
		m.vm.State = "draining"
	}
	m.vm.Generation++
	m.vm.UnknownOutcome = false
	m.vm.CurrentOperationID = ""
	m.op.Status = "succeeded"
	return m.vm, nil
}

func (m *memoryLedger) MarkRunnerVMRegistered(_ context.Context, _, id string, generation, runnerID int64, name string) (store.RunnerVM, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	if m.vm.ID != id || m.vm.Generation != generation || m.vm.State != "running" || m.vm.CleanupRequested {
		return store.RunnerVM{}, store.ErrConflict
	}
	m.vm.State = "registered"
	m.vm.Generation++
	m.vm.ExternalRunnerID = runnerID
	m.vm.ExternalRunnerName = name
	return m.vm, nil
}

func (m *memoryLedger) MarkRunnerVMDraining(_ context.Context, _, id string, generation int64) (store.RunnerVM, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	if m.vm.ID != id || m.vm.Generation != generation {
		return store.RunnerVM{}, store.ErrConflict
	}
	m.vm.CleanupRequested = true
	if m.vm.State == "running" || m.vm.State == "registered" {
		m.vm.State = "draining"
		m.vm.Generation++
	}
	return m.vm, nil
}

func (m *memoryLedger) ListUnresolvedRunnerVMs(_ context.Context, scaleSetID int64, _ int) ([]store.RunnerVM, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	if m.vm.UnknownOutcome && m.vm.ScaleSetID == scaleSetID {
		return []store.RunnerVM{m.vm}, nil
	}
	return nil, nil
}

type testMetadataResolver struct{}

func (testMetadataResolver) Resolve(_ context.Context, job github.Job) (Metadata, error) {
	return Metadata{WorkflowAttempt: 1, CommitSHA: testSHA, JobID: job.JobID, WorkflowRunID: job.WorkflowRunID, Repository: job.Owner + "/" + job.Repository}, nil
}

type testBootstrapper struct {
	calls    int
	receipts map[string]BootstrapReceipt
}

func (b *testBootstrapper) Prepare(_ context.Context, vm store.RunnerVM) (BootstrapReceipt, error) {
	b.calls++
	if receipt, ok := b.receipts[vm.ID]; ok {
		return receipt, nil
	}
	if vm.State != "running" || vm.UnknownOutcome {
		return BootstrapReceipt{}, errors.New("guest is not running and reconciled")
	}
	evidenceID, _ := store.NewID()
	receipt := BootstrapReceipt{ReservationID: vm.ID, VMID: vm.VMID, CommitSHA: vm.CommitSHA,
		GuestOS: "linux", GuestArchitecture: "amd64", ServiceAccount: "ra8ci",
		RunnerBinarySHA256: strings.Repeat("a", 64), AgentBinarySHA256: strings.Repeat("b", 64),
		ReadinessSHA256: strings.Repeat("c", 64), JITConfigSHA256: strings.Repeat("d", 64),
		JITConfigExpiresAt: time.Now().Add(time.Minute), EvidenceID: evidenceID, PreparedAt: time.Now()}
	if b.receipts == nil {
		b.receipts = make(map[string]BootstrapReceipt)
	}
	b.receipts[vm.ID] = receipt
	return receipt, nil
}

type failedBootstrap struct{}

func (failedBootstrap) Prepare(context.Context, store.RunnerVM) (BootstrapReceipt, error) {
	return BootstrapReceipt{}, errors.New("JIT credential channel unavailable")
}

type testObserver struct{}

func (testObserver) Registered(_ context.Context, _ store.RunnerVM, job github.Job) (RunnerObservation, error) {
	id, _ := store.NewID()
	return RunnerObservation{RunnerID: int64(job.RunnerID), RunnerName: job.RunnerName, EvidenceID: id, ObservedAt: time.Now()}, nil
}
func (testObserver) DrainAndDeregister(_ context.Context, vm store.RunnerVM, _ github.Job) (RunnerObservation, error) {
	id, _ := store.NewID()
	return RunnerObservation{RunnerID: vm.ExternalRunnerID, RunnerName: vm.ExternalRunnerName, EvidenceID: id, ObservedAt: time.Now(), Drained: true, NoActiveJob: true, RunnerDeregistered: true}, nil
}

type testBackupGate struct{ err error }

func (b testBackupGate) Check(_ context.Context, _ string) error { return b.err }

type fakeProxmox struct {
	mu                sync.Mutex
	exists            bool
	status            string
	marker            string
	taskRunning       bool
	startLostResponse bool
	cloneCalls        int
	startCalls        int
	stopCalls         int
	deleteCalls       int
}

func (f *fakeProxmox) serve(w http.ResponseWriter, r *http.Request) {
	f.mu.Lock()
	defer f.mu.Unlock()
	if r.Header.Get("Authorization") != "PVEAPIToken=scaler@pve!api=secret" {
		w.WriteHeader(http.StatusForbidden)
		return
	}
	w.Header().Set("Content-Type", "application/json")
	path := r.URL.Path
	switch {
	case r.Method == http.MethodGet && path == "/api2/json/cluster/resources":
		entries := []map[string]any{{"vmid": 9001, "type": "qemu", "node": "pve", "name": "ra8-lab-template", "pool": "ra8-tf-lab", "status": "stopped", "template": 1}}
		if f.exists {
			entries = append(entries, map[string]any{"vmid": 9000, "type": "qemu", "node": "pve", "name": "ra8-lab-ci-9000", "pool": "ra8-tf-lab", "status": f.status, "template": 0})
		}
		respond(w, entries)
	case r.Method == http.MethodGet && path == "/api2/json/nodes/pve/qemu/9001/config":
		respond(w, map[string]any{"name": "ra8-lab-template", "digest": testDigest, "template": 1})
	case r.Method == http.MethodGet && path == "/api2/json/nodes/pve/qemu/9000/config" && f.exists:
		respond(w, map[string]any{"name": "ra8-lab-ci-9000", "description": f.marker, "digest": testDigest, "protection": 0, "template": 0, "scsi0": "ra8-tf-lab:disk-9000"})
	case r.Method == http.MethodGet && path == "/api2/json/nodes/pve/qemu/9000/status/current" && f.exists:
		respond(w, map[string]any{"vmid": 9000, "status": f.status})
	case r.Method == http.MethodPost && path == "/api2/json/nodes/pve/qemu/9001/clone":
		f.cloneCalls++
		_ = r.ParseForm()
		f.exists = true
		f.status = "stopped"
		f.marker = r.Form.Get("description")
		respond(w, fakeUPID("qmclone"))
	case r.Method == http.MethodPost && path == "/api2/json/nodes/pve/qemu/9000/status/start":
		f.startCalls++
		f.status = "running"
		if f.startLostResponse {
			w.WriteHeader(http.StatusInternalServerError)
			return
		}
		respond(w, fakeUPID("qmstart"))
	case r.Method == http.MethodPost && path == "/api2/json/nodes/pve/qemu/9000/status/stop":
		f.stopCalls++
		f.status = "stopped"
		respond(w, fakeUPID("qmstop"))
	case r.Method == http.MethodDelete && path == "/api2/json/nodes/pve/qemu/9000":
		f.deleteCalls++
		f.exists = false
		respond(w, fakeUPID("qmdestroy"))
	case r.Method == http.MethodGet && strings.HasPrefix(path, "/api2/json/nodes/pve/tasks/UPID:") && strings.HasSuffix(path, "/status"):
		upid := strings.TrimSuffix(strings.TrimPrefix(path, "/api2/json/nodes/pve/tasks/"), "/status")
		parts := strings.Split(upid, ":")
		if len(parts) < 8 {
			w.WriteHeader(http.StatusBadRequest)
			return
		}
		state := "stopped"
		if f.taskRunning {
			state = "running"
		}
		respond(w, map[string]any{"upid": upid, "node": "pve", "id": parts[6], "type": parts[5], "status": state, "exitstatus": "OK"})
	default:
		w.WriteHeader(http.StatusNotFound)
	}
}

func respond(w http.ResponseWriter, data any) {
	_ = json.NewEncoder(w).Encode(map[string]any{"data": data})
}
func fakeUPID(kind string) string {
	return "UPID:pve:00000001:00000000:ABCD:" + kind + ":9000:api@pve!token:"
}

func mustDistinctID(t *testing.T, other string) string {
	t.Helper()
	id, err := store.NewID()
	if err != nil {
		t.Fatal(err)
	}
	if id == other {
		return mustDistinctID(t, other)
	}
	return id
}

func completedJob(job github.Job) github.Job {
	job.Kind = scaleset.MessageTypeJobCompleted
	job.Result = "Succeeded"
	job.FinishTime = time.Now().UTC()
	return job
}

func testHarness(t *testing.T) (*Handler, *memoryLedger, *fakeProxmox, *testBootstrapper, github.Job) {
	t.Helper()
	fake := &fakeProxmox{}
	server := httptest.NewTLSServer(http.HandlerFunc(fake.serve))
	t.Cleanup(server.Close)
	ca := filepath.Join(t.TempDir(), "ca.pem")
	if err := os.WriteFile(ca, pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: server.Certificate().Raw}), 0600); err != nil {
		t.Fatal(err)
	}
	token := filepath.Join(t.TempDir(), "token")
	if err := os.WriteFile(token, []byte("scaler@pve!api=secret"), 0600); err != nil {
		t.Fatal(err)
	}
	client, err := proxmox.New(proxmox.Config{Endpoint: server.URL, CAFile: ca, TokenFile: token, Node: "pve", Pool: "ra8-tf-lab", Storage: "ra8-tf-lab", AllowedVMIDs: []int{9000}, TemplateVMIDs: []int{9001}, RequestTimeout: time.Second, OperationTimeout: 70 * time.Millisecond, TaskPollInterval: time.Millisecond})
	if err != nil {
		t.Fatal(err)
	}
	approval, _ := store.NewID()
	config := Options{Actor: "github-scaler", ScaleSetID: 42, VMIDs: []int{9000}, Node: "pve", Pool: "ra8-tf-lab", Storage: "ra8-tf-lab", TemplateVMID: 9001, TemplateName: "ra8-lab-template", TemplateDigest: testDigest, BackupApprovalID: approval, CleanupApprovalID: mustDistinctID(t, approval)}
	policy, err := github.NewPolicy("bsikar", "ra8-firmware", []string{testWorkflow}, []string{"push"}, []string{"CI / test-go"}, []string{"ra8ci"})
	if err != nil {
		t.Fatal(err)
	}
	ledger := &memoryLedger{}
	bootstrap := &testBootstrapper{}
	handler, err := NewHandler(config, ledger, client, testMetadataResolver{}, bootstrap, testObserver{}, testBackupGate{}, policy)
	if err != nil {
		t.Fatal(err)
	}
	job := github.Job{Owner: "bsikar", Repository: "ra8-firmware", JobID: "job-1", WorkflowRunID: 23, RunnerRequestID: 19, WorkflowRef: testWorkflow, EventName: "push", DisplayName: "CI / test-go", Labels: []string{"ra8ci"}, RunnerID: 77, RunnerName: "runner-9000"}
	return handler, ledger, fake, bootstrap, job
}

func TestFullLifecycleUsesOneVMAndDeletesOnlyOwnedGuest(t *testing.T) {
	h, ledger, fake, bootstrap, job := testHarness(t)
	ctx := context.Background()
	if err := h.Process(ctx, github.Message{ScaleSetID: 42, Assigned: []github.Job{job}}); err != nil {
		t.Fatal(err)
	}
	if err := h.Process(ctx, github.Message{ScaleSetID: 42, Assigned: []github.Job{job}}); err != nil {
		t.Fatal(err)
	}
	vm, _ := ledger.GetRunnerVMByJob(ctx, 42, job.JobID)
	if vm.State != "running" || vm.UnknownOutcome || bootstrap.calls != 2 {
		t.Fatalf("assigned state=%+v bootstrap=%d", vm, bootstrap.calls)
	}
	if len(ledger.bootstrap) != 2 || ledger.bootstrap[1].ReservationID != vm.ID {
		t.Fatalf("bootstrap audit evidence=%+v", ledger.bootstrap)
	}
	if err := h.Process(ctx, github.Message{ScaleSetID: 42, Started: []github.Job{job}}); err != nil {
		t.Fatal(err)
	}
	if err := h.Process(ctx, github.Message{ScaleSetID: 42, Completed: []github.Job{completedJob(job)}}); err != nil {
		t.Fatal(err)
	}
	vm, _ = ledger.GetRunnerVMByJob(ctx, 42, job.JobID)
	if vm.State != "released" || !vm.CleanupRequested {
		t.Fatalf("not released: %+v", vm)
	}
	fake.mu.Lock()
	defer fake.mu.Unlock()
	if fake.cloneCalls != 1 || fake.startCalls != 1 || fake.stopCalls != 1 || fake.deleteCalls != 1 || fake.exists {
		t.Fatalf("VM calls clone=%d start=%d stop=%d delete=%d exists=%v", fake.cloneCalls, fake.startCalls, fake.stopCalls, fake.deleteCalls, fake.exists)
	}
}

func TestCrashAfterCloneReplaysMarkerWithoutSecondClone(t *testing.T) {
	h, ledger, fake, _, job := testHarness(t)
	ledger.loseUPIDOnce = true
	message := github.Message{ScaleSetID: 42, Assigned: []github.Job{job}}
	if err := h.Process(context.Background(), message); err == nil {
		t.Fatal("lost UPID not reported")
	}
	vm, _ := ledger.GetRunnerVMByJob(context.Background(), 42, job.JobID)
	if !vm.UnknownOutcome || vm.CurrentOperationID != vm.CreationOperationID {
		t.Fatalf("clone intent not durable: %+v", vm)
	}
	if err := h.Process(context.Background(), message); err != nil {
		t.Fatal(err)
	}
	fake.mu.Lock()
	defer fake.mu.Unlock()
	if fake.cloneCalls != 1 || fake.startCalls != 1 {
		t.Fatalf("replayed external mutation: clone=%d start=%d", fake.cloneCalls, fake.startCalls)
	}
}

func TestForeignMarkerCannotBeAdoptedOrDestroyed(t *testing.T) {
	h, ledger, fake, _, job := testHarness(t)
	fake.exists = true
	fake.status = "stopped"
	fake.marker = "RA8CI_RESERVATION=foreign;RA8CI_OPERATION=foreign"
	if err := h.Process(context.Background(), github.Message{ScaleSetID: 42, Assigned: []github.Job{job}}); err == nil {
		t.Fatal("foreign VM adopted")
	}
	vm, _ := ledger.GetRunnerVMByJob(context.Background(), 42, job.JobID)
	if !vm.UnknownOutcome {
		t.Fatalf("operation intent not retained: %+v", vm)
	}
	fake.mu.Lock()
	defer fake.mu.Unlock()
	if fake.cloneCalls != 0 || fake.startCalls != 0 || fake.deleteCalls != 0 {
		t.Fatal("foreign VM mutated")
	}
}

func TestTimedOutTaskReconcilesWithoutSecondMutation(t *testing.T) {
	h, ledger, fake, _, job := testHarness(t)
	fake.taskRunning = true
	msg := github.Message{ScaleSetID: 42, Assigned: []github.Job{job}}
	err := h.Process(context.Background(), msg)
	if !errors.Is(err, proxmox.ErrUnknownOutcome) {
		t.Fatalf("timeout not unknown: %v", err)
	}
	vm, _ := ledger.GetRunnerVMByJob(context.Background(), 42, job.JobID)
	if !vm.UnknownOutcome || vm.State != "cloning" {
		t.Fatalf("timeout not retained: %+v", vm)
	}
	fake.mu.Lock()
	fake.taskRunning = false
	fake.mu.Unlock()
	if err := h.Process(context.Background(), msg); err != nil {
		t.Fatal(err)
	}
	fake.mu.Lock()
	defer fake.mu.Unlock()
	if fake.cloneCalls != 1 || fake.startCalls != 1 {
		t.Fatalf("timeout caused duplicate mutation: clone=%d start=%d", fake.cloneCalls, fake.startCalls)
	}
}

func TestLostStartUPIDFailsClosed(t *testing.T) {
	h, ledger, fake, _, job := testHarness(t)
	fake.startLostResponse = true
	msg := github.Message{ScaleSetID: 42, Assigned: []github.Job{job}}
	if err := h.Process(context.Background(), msg); !errors.Is(err, proxmox.ErrUnknownOutcome) {
		t.Fatalf("lost start response not unknown: %v", err)
	}
	if err := h.Process(context.Background(), msg); !errors.Is(err, proxmox.ErrUnknownOutcome) {
		t.Fatalf("replay silently accepted lost start UPID: %v", err)
	}
	vm, _ := ledger.GetRunnerVMByJob(context.Background(), 42, job.JobID)
	if !vm.UnknownOutcome || vm.State != "starting" {
		t.Fatalf("lost start not retained: %+v", vm)
	}
	fake.mu.Lock()
	defer fake.mu.Unlock()
	if fake.startCalls != 1 {
		t.Fatalf("second start sent after unknown outcome: %d", fake.startCalls)
	}
}

func TestConstructorRejectsMissingApprovals(t *testing.T) {
	h, _, _, _, _ := testHarness(t)
	config := h.config
	config.BackupApprovalID = ""
	if _, err := NewHandler(config, h.ledger, h.vms, h.metadata, h.bootstrap, h.runners, h.backup, h.admission); err == nil {
		t.Fatal("missing backup approval accepted")
	}
	config = h.config
	config.CleanupApprovalID = config.BackupApprovalID
	if _, err := NewHandler(config, h.ledger, h.vms, h.metadata, h.bootstrap, h.runners, h.backup, h.admission); err == nil {
		t.Fatal("reused backup approval for cleanup accepted")
	}
	config = h.config
	config.VMIDs = []int{9000, 9000}
	if _, err := NewHandler(config, h.ledger, h.vms, h.metadata, h.bootstrap, h.runners, h.backup, h.admission); err == nil {
		t.Fatal("duplicate VMIDs accepted")
	}
	config = h.config
	if _, err := NewHandler(config, h.ledger, h.vms, h.metadata, nil, h.runners, h.backup, h.admission); err == nil {
		t.Fatal("missing JIT bootstrap accepted")
	}
	if err := h.Process(context.Background(), github.Message{ScaleSetID: 99}); err == nil {
		t.Fatal("foreign scale set accepted")
	}
}

func TestBadEventIdentityCannotBindOrCleanupRunner(t *testing.T) {
	h, ledger, fake, _, job := testHarness(t)
	ctx := context.Background()
	if err := h.Process(ctx, github.Message{ScaleSetID: 42, Assigned: []github.Job{job}}); err != nil {
		t.Fatal(err)
	}
	bad := job
	bad.WorkflowRunID++
	if err := h.Process(ctx, github.Message{ScaleSetID: 42, Started: []github.Job{bad}}); err == nil {
		t.Fatal("foreign started event accepted")
	}
	if err := h.Process(ctx, github.Message{ScaleSetID: 42, Completed: []github.Job{completedJob(bad)}}); err == nil {
		t.Fatal("foreign completion deleted runner")
	}
	vm, _ := ledger.GetRunnerVMByJob(ctx, 42, job.JobID)
	if vm.CleanupRequested || vm.State != "running" {
		t.Fatalf("foreign event changed state: %+v", vm)
	}
	fake.mu.Lock()
	defer fake.mu.Unlock()
	if fake.stopCalls != 0 || fake.deleteCalls != 0 {
		t.Fatal("foreign event mutated VM")
	}
}

func TestPostBootBootstrapFailureReplaysWithoutRecloningOrRestarting(t *testing.T) {
	h, ledger, fake, bootstrap, job := testHarness(t)
	h.bootstrap = failedBootstrap{}
	msg := github.Message{ScaleSetID: 42, Assigned: []github.Job{job}}
	if err := h.Process(context.Background(), msg); err == nil {
		t.Fatal("missing JIT did not fail")
	}
	vm, _ := ledger.GetRunnerVMByJob(context.Background(), 42, job.JobID)
	if vm.State != "running" || vm.UnknownOutcome {
		t.Fatalf("post-boot readiness failure state: %+v", vm)
	}
	fake.mu.Lock()
	cloneCalls, startCalls := fake.cloneCalls, fake.startCalls
	fake.mu.Unlock()
	if cloneCalls != 1 || startCalls != 1 {
		t.Fatal("post-boot readiness failure did not leave one reconciled running guest")
	}
	h.bootstrap = bootstrap
	if err := h.Process(context.Background(), msg); err != nil {
		t.Fatal(err)
	}
	fake.mu.Lock()
	defer fake.mu.Unlock()
	if fake.cloneCalls != 1 || fake.startCalls != 1 || bootstrap.calls != 1 {
		t.Fatal("readiness retry repeated VM mutations or did not resume bootstrap")
	}
}

func TestEarlyCompletionMonotonicallyFencesAssignment(t *testing.T) {
	h, ledger, fake, _, job := testHarness(t)
	input, err := h.jobInput(context.Background(), job, 9000)
	if err != nil {
		t.Fatal(err)
	}
	if _, _, err := ledger.ReserveRunnerVM(context.Background(), h.config.Actor, input); err != nil {
		t.Fatal(err)
	}
	if err := h.Process(context.Background(), github.Message{ScaleSetID: 42, Completed: []github.Job{completedJob(job)}}); err == nil {
		t.Fatal("early completion pretended to release unverified reservation")
	}
	vm, _ := ledger.GetRunnerVMByJob(context.Background(), 42, job.JobID)
	if !vm.CleanupRequested || vm.State != "reserved" {
		t.Fatalf("early completion not fenced: %+v", vm)
	}
	if err := h.Process(context.Background(), github.Message{ScaleSetID: 42, Assigned: []github.Job{job}}); err != nil {
		t.Fatal(err)
	}
	fake.mu.Lock()
	defer fake.mu.Unlock()
	if fake.cloneCalls != 0 || fake.startCalls != 0 {
		t.Fatal("late assignment started completed job")
	}
}

func TestBackupGateFailsClosedBeforeReserve(t *testing.T) {
	h, ledger, fake, _, job := testHarness(t)
	h.backup = testBackupGate{err: errors.New("off-VM backup stale")}
	if err := h.Process(context.Background(), github.Message{ScaleSetID: 42, Assigned: []github.Job{job}}); err == nil {
		t.Fatal("stale backup allowed reservation")
	}
	if ledger.reserveCalls != 0 {
		t.Fatal("VM reserved while backup stale")
	}
	fake.mu.Lock()
	defer fake.mu.Unlock()
	if fake.cloneCalls != 0 {
		t.Fatal("VM cloned while backup stale")
	}
}

func ExampleHandler() {
	fmt.Println("GitHub job -> durable VM reservation -> Proxmox task -> verified state")
	// Output: GitHub job -> durable VM reservation -> Proxmox task -> verified state
}
