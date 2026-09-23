// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package proxmox

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
)

const (
	testReservation = "0194f932-1c02-7000-8000-000000000001"
	testCreation    = "0194f932-1c02-7000-8000-000000000002"
	testAction      = "0194f932-1c02-7000-8000-000000000003"
	testEvidence    = "0194f932-1c02-7000-8000-000000000004"
	testApproval    = "0194f932-1c02-7000-8000-000000000005"
	testDigest      = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
)

var testIdentity = Identity{
	VMID: 9000, Node: "pve", Pool: "ra8-tf-lab", Storage: "ra8-tf-lab",
	Name: "ra8-lab-linux-example", ReservationID: testReservation, CreationOperationID: testCreation,
}

type fakePVE struct {
	mu                 sync.Mutex
	exists             bool
	status             string
	marker             string
	pool               string
	protected          bool
	lock               string
	taskRunning        bool
	failMutationStatus int
	redirect           string
	requests           []string
	form               map[string]string
	configOverride     map[string]any
	resourceOverride   map[string]any
	statusOverride     map[string]any
	taskOverride       map[string]any
	malformedResources bool
	templateDigest     string
}

func newFake() *fakePVE {
	return &fakePVE{status: "stopped", marker: testIdentity.marker(), pool: testIdentity.Pool}
}

func (f *fakePVE) serve(w http.ResponseWriter, r *http.Request) {
	f.mu.Lock()
	defer f.mu.Unlock()
	if r.Header.Get("Authorization") != "PVEAPIToken=ra8ci@pve!client=secret-token" {
		w.WriteHeader(http.StatusForbidden)
		return
	}
	f.requests = append(f.requests, r.Method+" "+r.URL.RequestURI())
	if f.redirect != "" && r.Method != http.MethodGet {
		http.Redirect(w, r, f.redirect, http.StatusFound)
		return
	}
	if f.failMutationStatus != 0 && r.Method != http.MethodGet {
		w.WriteHeader(f.failMutationStatus)
		return
	}
	w.Header().Set("Content-Type", "application/json")
	path := r.URL.Path
	switch {
	case r.Method == http.MethodGet && path == "/api2/json/cluster/resources":
		if f.malformedResources {
			_, _ = w.Write([]byte(`{"data":[]} {"data":[]}`))
			return
		}
		resources := []map[string]any{{"vmid": 9001, "type": "qemu", "node": "pve", "name": "ra8-lab-template", "pool": "ra8-tf-lab", "status": "stopped", "template": true}, {"vmid": 9199, "type": "qemu", "node": "pve", "name": "someone-else", "pool": "other", "status": "running", "template": 0}}
		if f.exists {
			entry := map[string]any{"vmid": 9000, "type": "qemu", "node": "pve", "name": testIdentity.Name, "pool": f.pool, "status": f.status, "template": 0}
			mergeMap(entry, f.resourceOverride)
			resources = append(resources, entry)
		}
		if r.URL.Query().Get("type") != "vm" {
			w.WriteHeader(http.StatusBadRequest)
			return
		}
		writeData(w, resources)
	case r.Method == http.MethodGet && path == "/api2/json/nodes/pve/qemu/9000/config" && f.exists:
		entry := map[string]any{"name": testIdentity.Name, "description": f.marker, "digest": testDigest, "protection": boolInt(f.protected), "template": 0, "lock": f.lock, "scsi0": "ra8-tf-lab:vm-9000-disk-0"}
		mergeMap(entry, f.configOverride)
		writeData(w, entry)
	case r.Method == http.MethodGet && path == "/api2/json/nodes/pve/qemu/9001/config":
		digest := testDigest
		if f.templateDigest != "" {
			digest = f.templateDigest
		}
		writeData(w, map[string]any{"name": "ra8-lab-template", "digest": digest, "template": 1})
	case r.Method == http.MethodGet && path == "/api2/json/nodes/pve/qemu/9000/status/current" && f.exists:
		entry := map[string]any{"vmid": 9000, "status": f.status}
		mergeMap(entry, f.statusOverride)
		writeData(w, entry)
	case r.Method == http.MethodPost && path == "/api2/json/nodes/pve/qemu/9001/clone":
		_ = r.ParseForm()
		f.form = map[string]string{}
		for key := range r.Form {
			f.form[key] = r.Form.Get(key)
		}
		f.exists = true
		f.status = "stopped"
		f.marker = r.Form.Get("description")
		writeData(w, fakeUPID("qmclone"))
	case r.Method == http.MethodPost && path == "/api2/json/nodes/pve/qemu/9000/status/start":
		f.status = "running"
		writeData(w, fakeUPID("qmstart"))
	case r.Method == http.MethodPost && path == "/api2/json/nodes/pve/qemu/9000/status/stop":
		f.status = "stopped"
		writeData(w, fakeUPID("qmstop"))
	case r.Method == http.MethodDelete && path == "/api2/json/nodes/pve/qemu/9000":
		if r.URL.Query().Get("purge") != "0" || r.URL.Query().Get("destroy-unreferenced-disks") != "0" {
			w.WriteHeader(http.StatusBadRequest)
			return
		}
		f.exists = false
		writeData(w, fakeUPID("qmdestroy"))
	case r.Method == http.MethodGet && strings.HasPrefix(path, "/api2/json/nodes/pve/tasks/UPID:") && strings.HasSuffix(path, "/status"):
		upid := strings.TrimSuffix(strings.TrimPrefix(path, "/api2/json/nodes/pve/tasks/"), "/status")
		parts := strings.Split(upid, ":")
		if len(parts) < 8 {
			w.WriteHeader(http.StatusBadRequest)
			return
		}
		if f.taskRunning {
			entry := map[string]any{"upid": upid, "node": "pve", "id": parts[6], "type": parts[5], "status": "running"}
			mergeMap(entry, f.taskOverride)
			writeData(w, entry)
		} else {
			entry := map[string]any{"upid": upid, "node": "pve", "id": parts[6], "type": parts[5], "status": "stopped", "exitstatus": "OK"}
			mergeMap(entry, f.taskOverride)
			writeData(w, entry)
		}
	default:
		w.WriteHeader(http.StatusNotFound)
	}
}

func mergeMap(dst, changes map[string]any) {
	for key, value := range changes {
		dst[key] = value
	}
}

func boolInt(value bool) int {
	if value {
		return 1
	}
	return 0
}

func fakeUPID(kind string) string {
	return "UPID:pve:00000001:00000000:ABCD:" + kind + ":9000:api@pve!token:"
}

func writeData(w http.ResponseWriter, data any) {
	_ = json.NewEncoder(w).Encode(map[string]any{"data": data})
}

func testClient(t *testing.T, f *fakePVE) (*Client, *httptest.Server) {
	t.Helper()
	server := httptest.NewTLSServer(http.HandlerFunc(f.serve))
	t.Cleanup(server.Close)
	cert := server.Certificate()
	if cert == nil {
		t.Fatal("test server has no certificate")
	}
	ca := filepath.Join(t.TempDir(), "ca.pem")
	if err := os.WriteFile(ca, pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: cert.Raw}), 0600); err != nil {
		t.Fatal(err)
	}
	token := filepath.Join(t.TempDir(), "token")
	if err := os.WriteFile(token, []byte("ra8ci@pve!client=secret-token\n"), 0600); err != nil {
		t.Fatal(err)
	}
	client, err := New(Config{Endpoint: server.URL, CAFile: ca, TokenFile: token, Node: "pve", Pool: "ra8-tf-lab", Storage: "ra8-tf-lab", AllowedVMIDs: []int{9000}, TemplateVMIDs: []int{9001}, RequestTimeout: time.Second, OperationTimeout: time.Second, TaskPollInterval: time.Millisecond})
	if err != nil {
		t.Fatal(err)
	}
	return client, server
}

func idleProof() IdleProof {
	return IdleProof{VMID: 9000, ReservationID: testReservation, EvidenceID: testEvidence, ObservedAt: time.Now(), Drained: true, NoActiveJob: true}
}

func TestLifecycleExactIdentityAndNoGenericMutation(t *testing.T) {
	f := newFake()
	client, _ := testClient(t, f)
	ctx := context.Background()
	listed, err := client.List(ctx)
	if err != nil || len(listed) != 0 {
		t.Fatalf("allowlist leaked a different VM: %+v, %v", listed, err)
	}
	clone, err := client.Clone(ctx, Action{ID: testCreation}, CloneSpec{Target: testIdentity, TemplateVMID: 9001, TemplateName: "ra8-lab-template", TemplateDigest: testDigest})
	if err != nil || clone.VM == nil || clone.VM.Status != "stopped" || clone.UPID == "" || clone.AlreadySatisfied {
		t.Fatalf("clone not verified: %+v, %v", clone, err)
	}
	for key, want := range map[string]string{"newid": "9000", "name": testIdentity.Name, "description": testIdentity.marker(), "full": "1", "pool": "ra8-tf-lab", "storage": "ra8-tf-lab"} {
		f.mu.Lock()
		got := f.form[key]
		f.mu.Unlock()
		if got != want {
			t.Fatalf("clone %s=%q, want %q", key, got, want)
		}
	}
	result, err := client.Clone(ctx, Action{ID: testCreation}, CloneSpec{Target: testIdentity, TemplateVMID: 9001, TemplateName: "ra8-lab-template", TemplateDigest: testDigest})
	if err != nil || !result.AlreadySatisfied {
		t.Fatalf("idempotent matching clone: %+v, %v", result, err)
	}
	observed, err := client.Reconcile(ctx, testCreation, testIdentity, "clone", "")
	if err != nil || !observed.AlreadySatisfied {
		t.Fatalf("marker reconciliation: %+v, %v", observed, err)
	}
	started, err := client.Start(ctx, Action{ID: testAction}, testIdentity)
	if err != nil || started.VM == nil || started.VM.Status != "running" {
		t.Fatalf("start not verified: %+v, %v", started, err)
	}
	startedAgain, err := client.Start(ctx, Action{ID: testAction}, testIdentity)
	if err != nil || !startedAgain.AlreadySatisfied {
		t.Fatalf("already-running start: %+v, %v", startedAgain, err)
	}
	stopped, err := client.Stop(ctx, Action{ID: testAction}, testIdentity, idleProof())
	if err != nil || stopped.VM == nil || stopped.VM.Status != "stopped" {
		t.Fatalf("stop not verified: %+v, %v", stopped, err)
	}
	proof := DestroyProof{IdleProof: idleProof(), ApprovalID: testApproval, ExpectedConfigDigest: testDigest, RunnerDeregistered: true, StateReconciled: true}
	destroyed, err := client.Destroy(ctx, Action{ID: testAction}, testIdentity, proof)
	if err != nil || destroyed.UPID == "" || destroyed.VM != nil {
		t.Fatalf("delete not verified: %+v, %v", destroyed, err)
	}
	f.mu.Lock()
	defer f.mu.Unlock()
	for _, request := range f.requests {
		if strings.Contains(request, "/config") && !strings.HasPrefix(request, "GET ") {
			t.Fatalf("generic configuration mutation escaped: %s", request)
		}
	}
}

func TestConflictAndDrainFailuresNeverMutate(t *testing.T) {
	f := newFake()
	f.exists = true
	f.marker = "someone-else"
	client, _ := testClient(t, f)
	_, err := client.Start(context.Background(), Action{ID: testAction}, testIdentity)
	if !errors.Is(err, ErrConflict) {
		t.Fatalf("wrong marker accepted: %v", err)
	}
	f.mu.Lock()
	f.marker = testIdentity.marker()
	f.protected = true
	f.mu.Unlock()
	proof := DestroyProof{IdleProof: idleProof(), ApprovalID: testApproval, ExpectedConfigDigest: testDigest, RunnerDeregistered: true, StateReconciled: true}
	_, err = client.Destroy(context.Background(), Action{ID: testAction}, testIdentity, proof)
	if !errors.Is(err, ErrConflict) {
		t.Fatalf("protected VM deleted: %v", err)
	}
	stale := idleProof()
	stale.ObservedAt = time.Now().Add(-time.Minute)
	_, err = client.Stop(context.Background(), Action{ID: testAction}, testIdentity, stale)
	if !errors.Is(err, ErrInvalid) {
		t.Fatalf("stale idle evidence accepted: %v", err)
	}
	_, err = client.Start(context.Background(), Action{ID: testAction, PriorRequestIssued: true}, testIdentity)
	if !errors.Is(err, ErrUnknownOutcome) {
		t.Fatalf("prior request retried: %v", err)
	}
	_, err = client.Clone(context.Background(), Action{ID: testAction}, CloneSpec{Target: testIdentity, TemplateVMID: 9002, TemplateName: "ra8-lab-template", TemplateDigest: testDigest})
	if !errors.Is(err, ErrInvalid) {
		t.Fatalf("unapproved template accepted: %v", err)
	}
	f.mu.Lock()
	defer f.mu.Unlock()
	for _, request := range f.requests {
		if !strings.HasPrefix(request, "GET ") {
			t.Fatalf("precondition issued mutation: %s", request)
		}
	}
}

func TestUnknownOutcomeAndReconciliation(t *testing.T) {
	f := newFake()
	client, _ := testClient(t, f)
	f.mu.Lock()
	f.failMutationStatus = http.StatusInternalServerError
	f.mu.Unlock()
	_, err := client.Clone(context.Background(), Action{ID: testCreation}, CloneSpec{Target: testIdentity, TemplateVMID: 9001, TemplateName: "ra8-lab-template", TemplateDigest: testDigest})
	if !errors.Is(err, ErrUnknownOutcome) {
		t.Fatalf("server error was not unknown: %v", err)
	}
	_, err = client.Reconcile(context.Background(), testAction, testIdentity, "start", "")
	if !errors.Is(err, ErrUnknownOutcome) {
		t.Fatalf("lost start UPID should remain unknown: %v", err)
	}
	f.mu.Lock()
	f.failMutationStatus = 0
	f.taskRunning = true
	f.mu.Unlock()
	client.operationTimeout = 25 * time.Millisecond
	_, err = client.Clone(context.Background(), Action{ID: testCreation}, CloneSpec{Target: testIdentity, TemplateVMID: 9001, TemplateName: "ra8-lab-template", TemplateDigest: testDigest})
	var unknown *UnknownOutcomeError
	if !errors.As(err, &unknown) || unknown.UPID == "" {
		t.Fatalf("bounded task timeout lost its UPID: %v", err)
	}
	f.mu.Lock()
	f.taskRunning = false
	f.mu.Unlock()
	client.operationTimeout = time.Second
	result, err := client.Reconcile(context.Background(), testCreation, testIdentity, "clone", unknown.UPID)
	if err != nil || result.VM == nil || result.UPID != unknown.UPID {
		t.Fatalf("UPID reconciliation: %+v, %v", result, err)
	}
}

func TestConstructorAndTransportFailClosed(t *testing.T) {
	f := newFake()
	client, server := testClient(t, f)
	config := Config{Endpoint: server.URL, CAFile: filepath.Join(t.TempDir(), "ca"), TokenEnv: "RA8CI_PROXMOX_API_TOKEN", Node: "pve", Pool: "ra8-tf-lab", Storage: "ra8-tf-lab", AllowedVMIDs: []int{9000}, TemplateVMIDs: []int{9001}}
	certBytes := pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: server.Certificate().Raw})
	if err := os.WriteFile(config.CAFile, certBytes, 0600); err != nil {
		t.Fatal(err)
	}
	t.Setenv(config.TokenEnv, "ra8ci@pve!client=secret-token")
	for _, tc := range []struct {
		name string
		edit func(*Config)
	}{
		{"http endpoint", func(c *Config) { c.Endpoint = "http://127.0.0.1:8006" }},
		{"personal tailscale", func(c *Config) { c.Endpoint = "https://personal.ts.net:8006" }},
		{"personal cgnat", func(c *Config) { c.Endpoint = "https://100.100.100.100:8006" }},
		{"unapproved id", func(c *Config) { c.AllowedVMIDs = []int{8999} }},
		{"duplicate id", func(c *Config) { c.AllowedVMIDs = []int{9000, 9000} }},
		{"both tokens", func(c *Config) { c.TokenFile = "also" }},
		{"wrong token env", func(c *Config) { c.TokenEnv = "HOME" }},
		{"bad timeout", func(c *Config) { c.OperationTimeout = 31 * time.Minute }},
	} {
		t.Run(tc.name, func(t *testing.T) {
			candidate := config
			tc.edit(&candidate)
			if _, err := New(candidate); !errors.Is(err, ErrInvalid) {
				t.Fatalf("invalid config accepted: %v", err)
			}
		})
	}
	badToken := filepath.Join(t.TempDir(), "world-readable")
	if err := os.WriteFile(badToken, []byte("ra8ci@pve!client=secret-token"), 0644); err != nil {
		t.Fatal(err)
	}
	config.TokenEnv = ""
	config.TokenFile = badToken
	if _, err := New(config); !errors.Is(err, ErrInvalid) {
		t.Fatalf("world-readable token accepted: %v", err)
	}
	if _, err := client.Get(context.Background(), Identity{VMID: 8999}); !errors.Is(err, ErrInvalid) {
		t.Fatalf("unapproved target queried: %v", err)
	}
	client.endpoint.Host = "127.0.0.1:1"
	if _, err := client.List(context.Background()); !errors.Is(err, ErrUnavailable) {
		t.Fatalf("unavailable endpoint accepted: %v", err)
	}
}

func TestRedirectAndMalformedResponseDoNotEscape(t *testing.T) {
	f := newFake()
	client, _ := testClient(t, f)
	gotCredential := false
	attacker := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		gotCredential = r.Header.Get("Authorization") != ""
		w.WriteHeader(http.StatusOK)
	}))
	defer attacker.Close()
	f.mu.Lock()
	f.redirect = attacker.URL
	f.mu.Unlock()
	_, err := client.Clone(context.Background(), Action{ID: testCreation}, CloneSpec{Target: testIdentity, TemplateVMID: 9001, TemplateName: "ra8-lab-template", TemplateDigest: testDigest})
	if !errors.Is(err, ErrUnknownOutcome) || gotCredential {
		t.Fatalf("redirect followed or misclassified: %v, credential=%v", err, gotCredential)
	}
	if !errors.Is(client.validateIdentity(Identity{VMID: 9000, Node: "pve", Pool: "ra8-tf-lab", Storage: "ra8-tf-lab", Name: "evil;shutdown", ReservationID: testReservation, CreationOperationID: testCreation}), ErrInvalid) {
		t.Fatal("untrusted name accepted")
	}
}

func TestReconcileRefusesUnknownWithoutUPID(t *testing.T) {
	f := newFake()
	client, _ := testClient(t, f)
	for _, kind := range []string{"start", "stop", "destroy"} {
		_, err := client.Reconcile(context.Background(), testAction, testIdentity, kind, "")
		if !errors.Is(err, ErrUnknownOutcome) {
			t.Fatalf("%s without UPID resolved: %v", kind, err)
		}
	}
	_, err := client.Reconcile(context.Background(), testCreation, testIdentity, "clone", "")
	if !errors.Is(err, ErrUnknownOutcome) {
		t.Fatalf("absent clone resolved: %v", err)
	}
	_, err = client.Reconcile(context.Background(), testAction, testIdentity, "bogus", "")
	if !errors.Is(err, ErrInvalid) {
		t.Fatalf("unknown action accepted: %v", err)
	}
}

func TestInspectionRejectsIdentityAndProtocolDrift(t *testing.T) {
	for _, tc := range []struct {
		name     string
		resource map[string]any
		config   map[string]any
		status   map[string]any
		want     error
	}{
		{name: "node", resource: map[string]any{"node": "other"}, want: ErrConflict},
		{name: "name", resource: map[string]any{"name": "someone-else"}, want: ErrConflict},
		{name: "pool", resource: map[string]any{"pool": "other"}, want: ErrConflict},
		{name: "template resource", resource: map[string]any{"template": 1}, want: ErrConflict},
		{name: "marker", config: map[string]any{"description": "wrong"}, want: ErrConflict},
		{name: "config name", config: map[string]any{"name": "someone-else"}, want: ErrConflict},
		{name: "digest", config: map[string]any{"digest": "bad"}, want: ErrProtocol},
		{name: "protection syntax", config: map[string]any{"protection": "bad"}, want: ErrProtocol},
		{name: "template config", config: map[string]any{"template": true}, want: ErrConflict},
		{name: "lock syntax", config: map[string]any{"lock": true}, want: ErrProtocol},
		{name: "disk storage", config: map[string]any{"scsi0": "other:vm-9000-disk-0"}, want: ErrConflict},
		{name: "disk syntax", config: map[string]any{"scsi0": 123}, want: ErrProtocol},
		{name: "vmid", status: map[string]any{"vmid": 9002}, want: ErrProtocol},
		{name: "status", status: map[string]any{"status": "paused"}, want: ErrProtocol},
	} {
		t.Run(tc.name, func(t *testing.T) {
			f := newFake()
			f.exists = true
			f.resourceOverride = tc.resource
			f.configOverride = tc.config
			f.statusOverride = tc.status
			client, _ := testClient(t, f)
			if _, err := client.Get(context.Background(), testIdentity); !errors.Is(err, tc.want) {
				t.Fatalf("Get rejected with %v, want %v", err, tc.want)
			}
		})
	}
}

func TestTaskIdentityAndFailureCannotVerifyMutation(t *testing.T) {
	for _, tc := range []struct {
		name     string
		override map[string]any
	}{
		{name: "wrong task vmid", override: map[string]any{"id": "9199"}},
		{name: "wrong task type", override: map[string]any{"type": "qmstop"}},
		{name: "wrong task node", override: map[string]any{"node": "elsewhere"}},
		{name: "wrong task upid", override: map[string]any{"upid": fakeUPID("qmstop")}},
		{name: "failed task", override: map[string]any{"exitstatus": "failed"}},
		{name: "invalid task state", override: map[string]any{"status": "queued"}},
	} {
		t.Run(tc.name, func(t *testing.T) {
			f := newFake()
			f.taskOverride = tc.override
			client, _ := testClient(t, f)
			_, err := client.Clone(context.Background(), Action{ID: testCreation}, CloneSpec{Target: testIdentity, TemplateVMID: 9001, TemplateName: "ra8-lab-template", TemplateDigest: testDigest})
			var unknown *UnknownOutcomeError
			if !errors.As(err, &unknown) || unknown.UPID == "" {
				t.Fatalf("task mismatch was considered known: %v", err)
			}
		})
	}
}

func TestDestructiveProofAndCloneOperationBinding(t *testing.T) {
	f := newFake()
	f.exists = true
	client, _ := testClient(t, f)
	ctx := context.Background()
	if _, err := client.Clone(ctx, Action{ID: testAction}, CloneSpec{Target: testIdentity, TemplateVMID: 9001, TemplateName: "ra8-lab-template", TemplateDigest: testDigest}); !errors.Is(err, ErrInvalid) {
		t.Fatalf("clone accepted unrelated operation marker: %v", err)
	}
	proof := DestroyProof{IdleProof: idleProof(), ApprovalID: testApproval, ExpectedConfigDigest: testDigest, RunnerDeregistered: true, StateReconciled: true}
	for _, tc := range []struct {
		name string
		edit func(*DestroyProof)
	}{
		{"wrong digest", func(p *DestroyProof) { p.ExpectedConfigDigest = strings.Repeat("b", 40) }},
		{"no approval", func(p *DestroyProof) { p.ApprovalID = "" }},
		{"registered runner", func(p *DestroyProof) { p.RunnerDeregistered = false }},
		{"unreconciled state", func(p *DestroyProof) { p.StateReconciled = false }},
	} {
		t.Run(tc.name, func(t *testing.T) {
			candidate := proof
			tc.edit(&candidate)
			if _, err := client.Destroy(ctx, Action{ID: testAction}, testIdentity, candidate); err == nil {
				t.Fatal("unsafe destruction accepted")
			}
		})
	}
	f.mu.Lock()
	defer f.mu.Unlock()
	for _, req := range f.requests {
		if strings.HasPrefix(req, "DELETE ") || strings.Contains(req, "/clone") {
			t.Fatalf("unsafe mutation issued: %s", req)
		}
	}
}

func TestListAndAlreadySatisfiedStop(t *testing.T) {
	f := newFake()
	f.exists = true
	client, _ := testClient(t, f)
	listed, err := client.List(context.Background())
	if err != nil || len(listed) != 1 || listed[0].Identity.VMID != testIdentity.VMID {
		t.Fatalf("allowlisted VM not listed: %+v, %v", listed, err)
	}
	stopped, err := client.Stop(context.Background(), Action{ID: testAction}, testIdentity, idleProof())
	if err != nil || !stopped.AlreadySatisfied {
		t.Fatalf("already-stopped VM mutated: %+v, %v", stopped, err)
	}
	f.mu.Lock()
	f.pool = "other"
	f.mu.Unlock()
	if _, err := client.List(context.Background()); !errors.Is(err, ErrConflict) {
		t.Fatalf("foreign-pool VM listed: %v", err)
	}
	f.mu.Lock()
	f.pool = testIdentity.Pool
	f.resourceOverride = map[string]any{"type": "lxc"}
	f.mu.Unlock()
	if _, err := client.List(context.Background()); !errors.Is(err, ErrConflict) {
		t.Fatalf("non-QEMU guest listed: %v", err)
	}
}

func TestReconcileCannotInventTaskResult(t *testing.T) {
	f := newFake()
	client, _ := testClient(t, f)
	if _, err := client.Reconcile(context.Background(), testAction, testIdentity, "start", "bad-upid"); !errors.Is(err, ErrInvalid) {
		t.Fatalf("malformed task ID accepted: %v", err)
	}
	if _, err := client.Reconcile(context.Background(), testAction, testIdentity, "start", fakeUPID("qmstart")); !errors.Is(err, ErrUnknownOutcome) {
		t.Fatalf("finished task without matching VM resolved: %v", err)
	}
	f.mu.Lock()
	f.exists = true
	f.status = "running"
	f.lock = "migrate"
	f.mu.Unlock()
	if _, err := client.Start(context.Background(), Action{ID: testAction}, testIdentity); !errors.Is(err, ErrConflict) {
		t.Fatalf("locked VM started: %v", err)
	}
}

func TestMalformedAPIEnvelopeIsRejected(t *testing.T) {
	f := newFake()
	f.malformedResources = true
	client, _ := testClient(t, f)
	if _, err := client.List(context.Background()); !errors.Is(err, ErrProtocol) {
		t.Fatalf("extra JSON object accepted: %v", err)
	}
}

func TestCloneRejectsChangedSourceTemplateDigest(t *testing.T) {
	f := newFake()
	f.templateDigest = strings.Repeat("b", 40)
	client, _ := testClient(t, f)
	_, err := client.Clone(context.Background(), Action{ID: testCreation}, CloneSpec{Target: testIdentity, TemplateVMID: 9001, TemplateName: "ra8-lab-template", TemplateDigest: testDigest})
	if !errors.Is(err, ErrConflict) {
		t.Fatalf("unreviewed template digest accepted: %v", err)
	}
	f.mu.Lock()
	defer f.mu.Unlock()
	for _, req := range f.requests {
		if strings.HasPrefix(req, "POST ") {
			t.Fatalf("clone began after template changed: %s", req)
		}
	}
}

func ExampleIdentity() {
	identity := testIdentity
	fmt.Println(identity.VMID, identity.Name)
	// Output: 9000 ra8-lab-linux-example
}
