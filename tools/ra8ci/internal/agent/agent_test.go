// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package agent

import (
	"context"
	"crypto/ed25519"
	"crypto/rand"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/json"
	"encoding/pem"
	"errors"
	"math/big"
	"net/http"
	"net/http/httptest"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"

	embedded "github.com/bsikar/ra8-firmware/tools/ra8ci/catalog"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/executor"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/protocol"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/source"
)

const (
	testAssignmentID = "01994d50-1234-7abc-8abc-0123456789ab"
	testAttemptID    = "01994d50-1234-7abc-8abc-0123456789ac"
)

func testAssignment() protocol.Assignment {
	return protocol.Assignment{SchemaVersion: protocol.Version, AssignmentID: testAssignmentID,
		AttemptID: testAttemptID, AssignmentVersion: 7, FencingToken: 11,
		Task:          protocol.TaskRef{Name: "format-check", Version: 1},
		CatalogSHA256: strings.Repeat("a", 64),
		Source: protocol.SourceRef{Algorithm: source.Algorithm, Commit: strings.Repeat("a", 40),
			SnapshotSHA256: strings.Repeat("b", 64)},
		DeadlineAt: time.Now().UTC().Add(time.Hour), RemainingMS: 60000}
}

func testAgent(t *testing.T, handler http.HandlerFunc) (*Agent, *httptest.Server) {
	t.Helper()
	server := httptest.NewServer(handler)
	definitions, err := catalog.Load()
	if err != nil {
		server.Close()
		t.Fatal(err)
	}
	return &Agent{base: server.URL, root: t.TempDir(), pollWait: time.Millisecond,
		client: server.Client(), catalog: definitions}, server
}

func TestAssignmentBudget(t *testing.T) {
	a := testAssignment()
	budget, err := assignmentBudget(a, 900)
	if err != nil || budget <= 59*time.Second || budget > 60*time.Second {
		t.Fatalf("budget = %v, %v", budget, err)
	}
	// A skewed host wall clock must not alter the server's monotonic hint.
	a.DeadlineAt = time.Now().Add(-time.Hour)
	budget, err = assignmentBudget(a, 900)
	if err != nil || budget != 59500*time.Millisecond {
		t.Fatalf("wall-clock skew changed budget = %v, %v", budget, err)
	}
	budget, err = assignmentBudget(a, 2)
	if err != nil || budget != 1500*time.Millisecond {
		t.Fatalf("reviewed task cap = %v, %v", budget, err)
	}
	if _, err := assignmentBudget(a, 0); !errors.Is(err, ErrUnsafeAssignment) {
		t.Fatalf("invalid catalog deadline accepted: %v", err)
	}
	a = testAssignment()
	a.RemainingMS = 400
	if _, err := assignmentBudget(a, 900); !errors.Is(err, ErrUnsafeAssignment) {
		t.Fatalf("expired server hint accepted: %v", err)
	}
}

func TestNewRejectsInsecureConfiguration(t *testing.T) {
	for _, config := range []Config{{ServerURL: "http://localhost:8080"},
		{ServerURL: "https://user:secret@host"}, {ServerURL: "https://host/path"},
		{ServerURL: "https://host"}} {
		if _, err := New(config); err == nil {
			t.Fatalf("unsafe config accepted: %+v", config)
		}
	}
}

func TestNewTLSIdentityAndRedirectPolicy(t *testing.T) {
	server := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		http.Redirect(w, r, "https://untrusted.example.invalid/", http.StatusTemporaryRedirect)
	}))
	defer server.Close()
	root := t.TempDir()
	caFile := filepath.Join(root, "ca.pem")
	certFile := filepath.Join(root, "client.pem")
	keyFile := filepath.Join(root, "client-key.pem")
	publicKey, privateKey, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	template := &x509.Certificate{SerialNumber: big.NewInt(1), Subject: pkix.Name{CommonName: "test-agent"},
		NotBefore: time.Now().Add(-time.Hour), NotAfter: time.Now().Add(time.Hour),
		KeyUsage: x509.KeyUsageDigitalSignature, ExtKeyUsage: []x509.ExtKeyUsage{x509.ExtKeyUsageClientAuth}}
	certDER, err := x509.CreateCertificate(rand.Reader, template, template, publicKey, privateKey)
	if err != nil {
		t.Fatal(err)
	}
	keyDER, err := x509.MarshalPKCS8PrivateKey(privateKey)
	if err != nil {
		t.Fatal(err)
	}
	for path, block := range map[string]*pem.Block{
		caFile:   {Type: "CERTIFICATE", Bytes: server.Certificate().Raw},
		certFile: {Type: "CERTIFICATE", Bytes: certDER},
		keyFile:  {Type: "PRIVATE KEY", Bytes: keyDER},
	} {
		if err := os.WriteFile(path, pem.EncodeToMemory(block), 0600); err != nil {
			t.Fatal(err)
		}
	}
	agent, err := New(Config{ServerURL: server.URL, CAFile: caFile, CertFile: certFile,
		KeyFile: keyFile, Root: root, PollWait: time.Millisecond})
	if err != nil {
		t.Fatal(err)
	}
	_, err = agent.post(context.Background(), "/v1/agents/me/claim", map[string]any{"schema_version": 1},
		&protocol.Assignment{}, false)
	if !errors.Is(err, ErrServerProtocol) || strings.Contains(err.Error(), "untrusted.example.invalid") {
		t.Fatalf("redirect followed or accepted: %v", err)
	}
	if _, err := New(Config{ServerURL: server.URL, CAFile: keyFile, CertFile: certFile,
		KeyFile: keyFile, Root: root}); err == nil {
		t.Fatal("non-certificate CA accepted")
	}
}

func TestRunOnceNoAssignmentAndMalformedGrant(t *testing.T) {
	var malformed bool
	agent, server := testAgent(t, func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost || r.URL.Path != "/v1/agents/me/claim" {
			t.Errorf("unexpected request: %s %s", r.Method, r.URL.Path)
			w.WriteHeader(http.StatusNotFound)
			return
		}
		var claim protocol.ClaimRequest
		if err := protocol.DecodeStrict(r.Body, &claim); err != nil || claim.Validate() != nil {
			t.Errorf("bad claim: %+v, %v", claim, err)
		}
		if !malformed {
			w.WriteHeader(http.StatusNoContent)
			return
		}
		w.Header().Set("Content-Type", "application/json")
		_ = json.NewEncoder(w).Encode(map[string]any{"schema_version": 1, "unknown": true})
	})
	defer server.Close()
	assigned, err := agent.RunOnce(context.Background())
	if err != nil || assigned {
		t.Fatalf("empty claim = %v, %v", assigned, err)
	}
	malformed = true
	assigned, err = agent.RunOnce(context.Background())
	if err == nil || assigned {
		t.Fatalf("malformed grant accepted: %v, %v", assigned, err)
	}
}

func TestRunOnceRejectsUnreviewedTaskWithoutAck(t *testing.T) {
	a := testAssignment()
	a.Task.Name = "arbitrary-shell"
	requests := 0
	agent, server := testAgent(t, func(w http.ResponseWriter, r *http.Request) {
		requests++
		if r.URL.Path != "/v1/agents/me/claim" {
			t.Errorf("unsafe task produced extra request: %s", r.URL.Path)
		}
		_ = json.NewEncoder(w).Encode(a)
	})
	defer server.Close()
	assigned, err := agent.RunOnce(context.Background())
	if !assigned || !errors.Is(err, ErrUnsafeAssignment) || requests != 1 {
		t.Fatalf("unsafe assignment = %v, %v, requests=%d", assigned, err, requests)
	}
}

func TestRunOnceRejectsSourceMismatchWithoutAck(t *testing.T) {
	root, snapshot := fixtureCheckout(t, "printf 'safe\\n'\n")
	a := testAssignment()
	definitions, err := catalog.Load()
	if err != nil {
		t.Fatal(err)
	}
	a.CatalogSHA256 = definitions.Digest()
	a.Source.Commit = snapshot.RootCommit
	a.Source.SnapshotSHA256 = strings.Repeat("0", 64)
	requests := 0
	agent, server := testAgent(t, func(w http.ResponseWriter, r *http.Request) {
		requests++
		if r.URL.Path != "/v1/agents/me/claim" {
			t.Errorf("source mismatch produced extra request: %s", r.URL.Path)
		}
		_ = json.NewEncoder(w).Encode(a)
	})
	defer server.Close()
	agent.root = root
	assigned, err := agent.RunOnce(context.Background())
	if !assigned || !errors.Is(err, ErrUnsafeAssignment) || requests != 1 {
		t.Fatalf("source mismatch = %v, %v, requests=%d", assigned, err, requests)
	}
}

func TestRunOnceRejectsExhaustedHintWithoutAck(t *testing.T) {
	a := testAssignment()
	definitions, err := catalog.Load()
	if err != nil {
		t.Fatal(err)
	}
	a.CatalogSHA256 = definitions.Digest()
	a.RemainingMS = 100
	requests := 0
	agent, server := testAgent(t, func(w http.ResponseWriter, r *http.Request) {
		requests++
		_ = json.NewEncoder(w).Encode(a)
	})
	defer server.Close()
	assigned, err := agent.RunOnce(context.Background())
	if !assigned || !errors.Is(err, ErrUnsafeAssignment) || requests != 1 {
		t.Fatalf("exhausted hint = %v, %v, requests=%d", assigned, err, requests)
	}
}

func TestRunOnceRejectsInvalidAndWritableGrants(t *testing.T) {
	definitions, err := catalog.Load()
	if err != nil {
		t.Fatal(err)
	}
	for _, mutation := range []func(*protocol.Assignment){
		func(a *protocol.Assignment) { a.SchemaVersion = 2 },
		func(a *protocol.Assignment) { a.Task.Name = "format"; a.CatalogSHA256 = definitions.Digest() },
	} {
		a := testAssignment()
		mutation(&a)
		requests := 0
		agent, server := testAgent(t, func(w http.ResponseWriter, r *http.Request) {
			requests++
			_ = json.NewEncoder(w).Encode(a)
		})
		assigned, err := agent.RunOnce(context.Background())
		server.Close()
		if !assigned || !errors.Is(err, ErrUnsafeAssignment) || requests != 1 {
			t.Fatalf("unsafe grant = %v, %v, requests=%d", assigned, err, requests)
		}
	}
}

func TestLogUploaderChunksAndAmbiguousRetry(t *testing.T) {
	a := testAssignment()
	var mu sync.Mutex
	seen := map[int64]int{}
	failSecond := true
	agent, server := testAgent(t, func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/v1/attempts/"+a.AttemptID+"/logs" {
			w.WriteHeader(http.StatusNotFound)
			return
		}
		var chunk protocol.LogChunk
		if err := protocol.DecodeStrict(r.Body, &chunk); err != nil || chunk.Validate() != nil {
			t.Errorf("invalid log chunk: %+v, %v", chunk, err)
		}
		mu.Lock()
		seen[chunk.Sequence]++
		shouldFail := chunk.Sequence == 2 && failSecond
		if shouldFail {
			failSecond = false
		}
		mu.Unlock()
		if shouldFail {
			w.WriteHeader(http.StatusServiceUnavailable)
			return
		}
		_ = json.NewEncoder(w).Encode(protocol.AcceptResponse{SchemaVersion: protocol.Version,
			AssignmentVersion: a.AssignmentVersion, FencingToken: a.FencingToken, Accepted: true})
	})
	defer server.Close()
	uploader := &logUploader{agent: agent, ctx: context.Background(), assignment: a}
	data := []byte(strings.Repeat("x", protocol.MaxLogBytes+4))
	n, err := uploader.write("step-one", "stdout", data)
	if n != protocol.MaxLogBytes || !errors.Is(err, ErrServerProtocol) {
		t.Fatalf("partial upload = %d, %v", n, err)
	}
	uploader.flushGrace(context.Background())
	sequence, evidenceErr := uploader.status()
	if sequence != 2 || evidenceErr == nil {
		t.Fatalf("retry result = %d, %v", sequence, evidenceErr)
	}
	mu.Lock()
	defer mu.Unlock()
	if seen[1] != 1 || seen[2] != 2 {
		t.Fatalf("sequences = %+v", seen)
	}
}

func TestTerminalReceiptPreservesFailureEvidence(t *testing.T) {
	a := testAssignment()
	facts, err := HostFacts()
	if err != nil {
		t.Fatal(err)
	}
	now := time.Now().UTC()
	result := executor.Result{TaskName: "format-check", StartedAt: now, EndedAt: now.Add(time.Second),
		Duration: time.Second, ExitCode: 17, Steps: []executor.StepResult{{Name: "format-tree-check",
			StartedAt: now, EndedAt: now.Add(time.Second), Duration: time.Second, ExitCode: 17}}}
	receipt := terminalReceipt(a, result, facts, facts, 3, nil, nil)
	if receipt.Outcome != "failed" || receipt.ChildExitCode == nil || *receipt.ChildExitCode != 17 ||
		!receipt.EvidenceComplete || receipt.FinalLogSequence != 3 || receipt.Validate() != nil {
		t.Fatalf("bad child failure receipt: %+v", receipt)
	}
	receipt = terminalReceipt(a, result, facts, facts, 3, errors.New("broken logs"), nil)
	if receipt.EvidenceComplete || receipt.ErrorCode != "executor_error" || receipt.Validate() != nil {
		t.Fatalf("false complete evidence: %+v", receipt)
	}
	result = executor.Result{}
	receipt = terminalReceipt(a, result, facts, facts, 0, errors.New("pre-start error"), nil)
	if receipt.ChildExitCode != nil || receipt.Outcome != "failed" || receipt.Validate() != nil {
		t.Fatalf("pre-start error forged exit: %+v", receipt)
	}
	result = executor.Result{StartedAt: now, EndedAt: now, ExitCode: -1, TimedOut: true}
	receipt = terminalReceipt(a, result, facts, facts, 0, nil, nil)
	if receipt.Outcome != "timed_out" || receipt.EvidenceComplete || receipt.ErrorCode != "no_step_executed" || receipt.Validate() != nil {
		t.Fatalf("pre-step timeout lost: %+v", receipt)
	}
}

func TestPostRejectsStaleAndUnknownResponses(t *testing.T) {
	a := testAssignment()
	stale := false
	agent, server := testAgent(t, func(w http.ResponseWriter, r *http.Request) {
		if stale {
			_ = json.NewEncoder(w).Encode(map[string]any{"schema_version": 1, "unknown": true})
			return
		}
		_ = json.NewEncoder(w).Encode(protocol.AcceptResponse{SchemaVersion: protocol.Version,
			AssignmentVersion: a.AssignmentVersion, FencingToken: a.FencingToken + 1, Accepted: true})
	})
	defer server.Close()
	if err := agent.accept(context.Background(), a, "/ack", map[string]string{"hello": "world"}); !errors.Is(err, ErrServerProtocol) {
		t.Fatalf("stale fence accepted: %v", err)
	}
	stale = true
	if err := agent.accept(context.Background(), a, "/ack", map[string]string{"hello": "world"}); !errors.Is(err, ErrServerProtocol) {
		t.Fatalf("unknown response accepted: %v", err)
	}
}

func TestPostRejectsHTTPFailureAndMissingBody(t *testing.T) {
	status := http.StatusServiceUnavailable
	body := ""
	agent, server := testAgent(t, func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(status)
		_, _ = w.Write([]byte(body))
	})
	defer server.Close()
	var response protocol.Assignment
	if _, err := agent.post(context.Background(), "/claim", map[string]string{}, &response, false); !errors.Is(err, ErrServerProtocol) {
		t.Fatalf("HTTP failure accepted: %v", err)
	}
	status = http.StatusOK
	if _, err := agent.post(context.Background(), "/claim", map[string]string{}, &response, false); !errors.Is(err, ErrServerProtocol) {
		t.Fatalf("empty 200 accepted: %v", err)
	}
	body = `{}`
	if _, err := agent.post(context.Background(), "/claim", map[string]string{}, nil, false); !errors.Is(err, ErrServerProtocol) {
		t.Fatalf("missing response target accepted: %v", err)
	}
}

func fixtureCheckout(t *testing.T, script string) (string, source.Result) {
	t.Helper()
	root := t.TempDir()
	files := map[string][]byte{
		"tools/ra8ci/catalog/tasks.json": embedded.Manifest(),
		"tools/ra8ci/catalog/sha256.txt": embedded.Digest(),
		"scripts/checks/format_tree.sh":  []byte(script),
	}
	for relative, data := range files {
		path := filepath.Join(root, relative)
		if err := os.MkdirAll(filepath.Dir(path), 0755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(path, data, 0644); err != nil {
			t.Fatal(err)
		}
	}
	for _, args := range [][]string{{"init", "-q"}, {"add", "."},
		{"-c", "user.name=Test", "-c", "user.email=test@example.invalid", "commit", "-qm", "fixture"}} {
		cmd := exec.Command("git", args...)
		cmd.Dir = root
		if output, err := cmd.CombinedOutput(); err != nil {
			t.Fatalf("git %v: %v: %s", args, err, output)
		}
	}
	snapshot, err := source.Snapshot(context.Background(), root)
	if err != nil {
		t.Fatal(err)
	}
	return root, snapshot
}

func TestRunOnceEndToEndReadOnly(t *testing.T) {
	root, snapshot := fixtureCheckout(t, "printf 'agent-log\\n'\n")
	a := testAssignment()
	definitions, err := catalog.Load()
	if err != nil {
		t.Fatal(err)
	}
	a.CatalogSHA256 = definitions.Digest()
	a.Source.Commit, a.Source.SnapshotSHA256 = snapshot.RootCommit, snapshot.Digest
	var mu sync.Mutex
	sequence := int64(0)
	terminal := protocol.TerminalReceipt{}
	seenAck := false
	agent, server := testAgent(t, func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/v1/agents/me/claim":
			_ = json.NewEncoder(w).Encode(a)
		case "/v1/assignments/" + a.AssignmentID + "/ack":
			var ack protocol.Ack
			if err := protocol.DecodeStrict(r.Body, &ack); err != nil || ack.Validate() != nil {
				t.Errorf("invalid ack: %+v, %v", ack, err)
			}
			mu.Lock()
			seenAck = true
			mu.Unlock()
			writeAccepted(w, a)
		case "/v1/attempts/" + a.AttemptID + "/logs":
			var chunk protocol.LogChunk
			if err := protocol.DecodeStrict(r.Body, &chunk); err != nil || chunk.Validate() != nil {
				t.Errorf("invalid log: %+v, %v", chunk, err)
			}
			mu.Lock()
			sequence = chunk.Sequence
			mu.Unlock()
			writeAccepted(w, a)
		case "/v1/attempts/" + a.AttemptID + "/result":
			var receipt protocol.TerminalReceipt
			if err := protocol.DecodeStrict(r.Body, &receipt); err != nil || receipt.Validate() != nil {
				t.Errorf("invalid terminal: %+v, %v", receipt, err)
			}
			mu.Lock()
			terminal = receipt
			mu.Unlock()
			writeAccepted(w, a)
		default:
			t.Errorf("unexpected endpoint %s", r.URL.Path)
			w.WriteHeader(http.StatusNotFound)
		}
	})
	defer server.Close()
	agent.root = root
	assigned, err := agent.RunOnce(context.Background())
	if err != nil || !assigned {
		t.Fatalf("run = %v, %v", assigned, err)
	}
	mu.Lock()
	defer mu.Unlock()
	if !seenAck || sequence != 1 || terminal.FinalLogSequence != 1 || terminal.Outcome != "succeeded" ||
		terminal.ChildExitCode == nil || *terminal.ChildExitCode != 0 || !terminal.EvidenceComplete {
		t.Fatalf("incomplete end-to-end evidence: ack=%v sequence=%d terminal=%+v", seenAck, sequence, terminal)
	}
}

func TestRunOnceDeadlineReportsTerminal(t *testing.T) {
	root, snapshot := fixtureCheckout(t, "sleep 5\n")
	a := testAssignment()
	definitions, err := catalog.Load()
	if err != nil {
		t.Fatal(err)
	}
	a.CatalogSHA256 = definitions.Digest()
	a.Source.Commit, a.Source.SnapshotSHA256 = snapshot.RootCommit, snapshot.Digest
	a.RemainingMS = 800
	a.DeadlineAt = time.Now().Add(3 * time.Second)
	var mu sync.Mutex
	terminal := protocol.TerminalReceipt{}
	agent, server := testAgent(t, func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/v1/agents/me/claim":
			_ = json.NewEncoder(w).Encode(a)
		case "/v1/assignments/" + a.AssignmentID + "/ack":
			writeAccepted(w, a)
		case "/v1/attempts/" + a.AttemptID + "/result":
			var receipt protocol.TerminalReceipt
			if err := protocol.DecodeStrict(r.Body, &receipt); err != nil {
				t.Error(err)
			}
			mu.Lock()
			terminal = receipt
			mu.Unlock()
			writeAccepted(w, a)
		default:
			t.Errorf("unexpected endpoint %s", r.URL.Path)
			w.WriteHeader(http.StatusNotFound)
		}
	})
	defer server.Close()
	agent.root = root
	assigned, runErr := agent.RunOnce(context.Background())
	if !assigned || runErr != nil {
		t.Fatalf("deadline run = %v, %v", assigned, runErr)
	}
	mu.Lock()
	defer mu.Unlock()
	if terminal.Outcome != "timed_out" || !terminal.TimedOut || terminal.Validate() != nil {
		t.Fatalf("deadline receipt absent/invalid: %+v", terminal)
	}
}

func TestHeartbeatFencedCancellation(t *testing.T) {
	a := testAssignment()
	requests := 0
	agent, server := testAgent(t, func(w http.ResponseWriter, r *http.Request) {
		requests++
		if r.URL.Path != "/v1/agents/me/heartbeat" {
			t.Errorf("unexpected heartbeat endpoint %s", r.URL.Path)
		}
		var heartbeat protocol.Heartbeat
		if err := protocol.DecodeStrict(r.Body, &heartbeat); err != nil || heartbeat.Validate() != nil {
			t.Errorf("bad heartbeat: %+v, %v", heartbeat, err)
		}
		_ = json.NewEncoder(w).Encode(protocol.HeartbeatResponse{SchemaVersion: protocol.Version,
			AssignmentVersion: a.AssignmentVersion, FencingToken: a.FencingToken, Cancel: true})
	})
	defer server.Close()
	ctx, cancel := context.WithTimeout(context.Background(), 7*time.Second)
	defer cancel()
	if err := agent.heartbeat(ctx, a, cancel); err != nil || requests != 1 || ctx.Err() == nil {
		t.Fatalf("heartbeat cancel = %v, requests=%d, ctx=%v", err, requests, ctx.Err())
	}
}

func TestHeartbeatRejectsStaleFence(t *testing.T) {
	a := testAssignment()
	agent, server := testAgent(t, func(w http.ResponseWriter, r *http.Request) {
		_ = json.NewEncoder(w).Encode(protocol.HeartbeatResponse{SchemaVersion: protocol.Version,
			AssignmentVersion: a.AssignmentVersion, FencingToken: a.FencingToken + 1})
	})
	defer server.Close()
	ctx, cancel := context.WithTimeout(context.Background(), 7*time.Second)
	defer cancel()
	if err := agent.heartbeat(ctx, a, cancel); !errors.Is(err, protocol.ErrInvalid) || ctx.Err() == nil {
		t.Fatalf("stale heartbeat intent accepted: %v, ctx=%v", err, ctx.Err())
	}
}

func TestRunReturnsCancellation(t *testing.T) {
	agent, server := testAgent(t, func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusNoContent)
	})
	defer server.Close()
	ctx, cancel := context.WithTimeout(context.Background(), 40*time.Millisecond)
	defer cancel()
	if err := agent.Run(ctx); !errors.Is(err, context.DeadlineExceeded) {
		t.Fatalf("run returned %v", err)
	}
}

func writeAccepted(w http.ResponseWriter, assignment protocol.Assignment) {
	_ = json.NewEncoder(w).Encode(protocol.AcceptResponse{SchemaVersion: protocol.Version,
		AssignmentVersion: assignment.AssignmentVersion, FencingToken: assignment.FencingToken, Accepted: true})
}
