// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

// Package agent runs reviewed read-only tasks from outbound mTLS assignments.
// Each agent checkout must be clean and dedicated to the agent. Write tasks and
// board actions are not dispatched until their own fenced protocols exist.
package agent

import (
	"bytes"
	"context"
	"crypto/sha256"
	"crypto/tls"
	"crypto/x509"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"sync"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/executor"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/protocol"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/source"
)

const (
	requestLimit      = 10 * time.Second
	heartbeatInterval = 5 * time.Second
	deadlineSafety    = 500 * time.Millisecond
	defaultPollWait   = 25 * time.Second
	maxHTTPResponse   = protocol.MaxJSONBytes
)

var (
	ErrUnsafeAssignment = errors.New("agent rejected unsafe assignment")
	ErrServerProtocol   = errors.New("agent server protocol failure")
)

// Config requires an HTTPS server and a dedicated client certificate. The
// agent identity is authenticated by that certificate, never a body field.
type Config struct {
	ServerURL string
	CAFile    string
	CertFile  string
	KeyFile   string
	Root      string
	PollWait  time.Duration
}

// Agent has no listener, incoming shell endpoint, or board device access.
type Agent struct {
	base     string
	root     string
	pollWait time.Duration
	client   *http.Client
	catalog  *catalog.Catalog
}

// New constructs an outbound-only client with TLS 1.3 mutual authentication.
func New(config Config) (*Agent, error) {
	parsed, err := url.Parse(config.ServerURL)
	if err != nil || parsed.Scheme != "https" || parsed.Host == "" || parsed.User != nil ||
		parsed.RawQuery != "" || parsed.Fragment != "" || (parsed.Path != "" && parsed.Path != "/") {
		return nil, fmt.Errorf("%w: server must be an HTTPS origin", ErrServerProtocol)
	}
	if config.CAFile == "" || config.CertFile == "" || config.KeyFile == "" || config.Root == "" {
		return nil, fmt.Errorf("%w: TLS identity and checkout are required", ErrServerProtocol)
	}
	caPEM, err := os.ReadFile(config.CAFile)
	if err != nil {
		return nil, err
	}
	roots := x509.NewCertPool()
	if !roots.AppendCertsFromPEM(caPEM) {
		return nil, fmt.Errorf("%w: invalid server CA", ErrServerProtocol)
	}
	identity, err := tls.LoadX509KeyPair(config.CertFile, config.KeyFile)
	if err != nil {
		return nil, err
	}
	absolute, err := filepath.Abs(config.Root)
	if err != nil {
		return nil, err
	}
	root, err := filepath.EvalSymlinks(absolute)
	if err != nil {
		return nil, err
	}
	definitions, err := catalog.Load()
	if err != nil {
		return nil, err
	}
	wait := config.PollWait
	if wait == 0 {
		wait = defaultPollWait
	}
	if wait < 0 || wait > 25*time.Second {
		return nil, fmt.Errorf("%w: invalid poll wait", ErrServerProtocol)
	}
	transport := &http.Transport{Proxy: nil, ForceAttemptHTTP2: true, TLSClientConfig: &tls.Config{
		MinVersion: tls.VersionTLS13, RootCAs: roots, Certificates: []tls.Certificate{identity},
	}}
	return &Agent{base: strings.TrimSuffix(config.ServerURL, "/"), root: root, pollWait: wait,
		client: &http.Client{Transport: transport, CheckRedirect: func(_ *http.Request, _ []*http.Request) error {
			return http.ErrUseLastResponse
		}}, catalog: definitions}, nil
}

// Run polls until cancelled. A protocol or active-attempt error terminates the
// loop so the service manager can restart while the server fences that attempt.
func (agent *Agent) Run(ctx context.Context) error {
	for ctx.Err() == nil {
		assigned, err := agent.RunOnce(ctx)
		if err != nil {
			return err
		}
		if !assigned {
			timer := time.NewTimer(250 * time.Millisecond)
			select {
			case <-ctx.Done():
				timer.Stop()
			case <-timer.C:
			}
		}
	}
	return ctx.Err()
}

// RunOnce claims at most one attempt; false means the server had no work.
func (agent *Agent) RunOnce(ctx context.Context) (bool, error) {
	if agent == nil || ctx == nil {
		return false, fmt.Errorf("%w: nil agent or context", ErrServerProtocol)
	}
	facts, err := HostFacts()
	if err != nil {
		return false, err
	}
	claim := protocol.ClaimRequest{SchemaVersion: protocol.Version, HostFacts: facts,
		PollWaitMS: agent.pollWait.Milliseconds()}
	if err := claim.Validate(); err != nil {
		return false, err
	}
	claimCtx, stop := context.WithTimeout(ctx, agent.pollWait+requestLimit)
	defer stop()
	var assignment protocol.Assignment
	status, err := agent.post(claimCtx, "/v1/agents/me/claim", claim, &assignment, true)
	if err != nil {
		return false, err
	}
	if status == http.StatusNoContent {
		return false, nil
	}
	if err := assignment.Validate(); err != nil {
		return true, fmt.Errorf("%w: invalid grant: %v", ErrUnsafeAssignment, err)
	}
	return true, agent.execute(ctx, assignment)
}

func (agent *Agent) execute(parent context.Context, assignment protocol.Assignment) error {
	task, found := agent.catalog.Task(assignment.Task.Name)
	if !found || task.Version != assignment.Task.Version ||
		assignment.CatalogSHA256 != agent.catalog.Digest() || task.Scope != "safe-local-read-only" ||
		task.BoardPolicy != "none" || !task.SupportsOS(runtime.GOOS) {
		return fmt.Errorf("%w: task is not an embedded read-only non-board definition", ErrUnsafeAssignment)
	}
	budget, err := assignmentBudget(assignment, task.DeadlineSeconds)
	if err != nil {
		return err
	}
	ctx, cancel := context.WithTimeout(parent, budget)
	defer cancel()
	if _, err := catalog.VerifyCheckout(agent.root); err != nil {
		return fmt.Errorf("%w: catalog checkout: %v", ErrUnsafeAssignment, err)
	}
	if _, err := source.Verify(ctx, agent.root, assignment.Source.Commit, assignment.Source.SnapshotSHA256); err != nil {
		return fmt.Errorf("%w: source snapshot: %v", ErrUnsafeAssignment, err)
	}
	startFacts, err := HostFacts()
	if err != nil {
		return err
	}
	ack := protocol.Ack{SchemaVersion: protocol.Version, AssignmentID: assignment.AssignmentID,
		AttemptID: assignment.AttemptID, AssignmentVersion: assignment.AssignmentVersion,
		FencingToken: assignment.FencingToken, CatalogSHA256: assignment.CatalogSHA256,
		SourceSnapshotSHA256: assignment.Source.SnapshotSHA256, HostFacts: startFacts}
	if err := ack.Validate(); err != nil {
		return err
	}
	if err := agent.accept(ctx, assignment, "/v1/assignments/"+assignment.AssignmentID+"/ack", ack); err != nil {
		return err
	}
	uploader := &logUploader{agent: agent, ctx: ctx, assignment: assignment}
	hbCtx, hbCancel := context.WithCancel(ctx)
	hbDone := make(chan error, 1)
	go func() { hbDone <- agent.heartbeat(hbCtx, assignment, cancel) }()
	result, runErr := executor.Run(ctx, agent.root, task, &streamWriter{uploader, "stdout"}, &streamWriter{uploader, "stderr"})
	hbCancel()
	hbErr := <-hbDone
	runErr = errors.Join(runErr, hbErr)
	// Retry only the last ambiguous log chunk. The server must treat the same
	// sequence and digest idempotently; this never resumes the child process.
	evidenceCtx, evidenceCancel := context.WithTimeout(parent, requestLimit)
	defer evidenceCancel()
	uploader.flushGrace(evidenceCtx)
	endFacts, err := HostFacts()
	if err != nil {
		return err
	}
	sequence, logErr := uploader.status()
	receipt := terminalReceipt(assignment, result, startFacts, endFacts, sequence, runErr, logErr)
	if err := receipt.Validate(); err != nil {
		return err
	}
	// This grace permits reporting a deadline, not further task execution.
	if err := agent.accept(evidenceCtx, assignment, "/v1/attempts/"+assignment.AttemptID+"/result", receipt); err != nil {
		return err
	}
	return errors.Join(runErr, logErr)
}

// assignmentBudget uses only the server's remaining-time hint for a local
// monotonic timer. DeadlineAt remains the server's audit/deadline authority;
// comparing it with this host's wall clock would assume clock synchronization.
func assignmentBudget(assignment protocol.Assignment, catalogDeadlineSeconds int) (time.Duration, error) {
	if err := assignment.Validate(); err != nil {
		return 0, fmt.Errorf("%w: %v", ErrUnsafeAssignment, err)
	}
	if catalogDeadlineSeconds < 1 || catalogDeadlineSeconds > protocol.MaxDeadlineMS/1000 {
		return 0, fmt.Errorf("%w: invalid reviewed task deadline", ErrUnsafeAssignment)
	}
	remaining := time.Duration(assignment.RemainingMS)*time.Millisecond - deadlineSafety
	maximum := time.Duration(catalogDeadlineSeconds)*time.Second - deadlineSafety
	if maximum < remaining {
		remaining = maximum
	}
	if remaining <= 0 {
		return 0, fmt.Errorf("%w: assignment deadline is exhausted", ErrUnsafeAssignment)
	}
	return remaining, nil
}

func (agent *Agent) post(ctx context.Context, endpoint string, request any, response any, allowEmpty bool) (int, error) {
	data, err := json.Marshal(request)
	if err != nil {
		return 0, err
	}
	limit := requestLimit
	if endpoint == "/v1/agents/me/claim" {
		limit += agent.pollWait
	}
	callCtx, cancel := context.WithTimeout(ctx, limit)
	defer cancel()
	call, err := http.NewRequestWithContext(callCtx, http.MethodPost, agent.base+endpoint, bytes.NewReader(data))
	if err != nil {
		return 0, err
	}
	call.Header.Set("Content-Type", "application/json")
	call.Header.Set("Accept", "application/json")
	result, err := agent.client.Do(call)
	if err != nil {
		return 0, fmt.Errorf("%w: request failed: %w", ErrServerProtocol, err)
	}
	defer result.Body.Close()
	if result.StatusCode == http.StatusNoContent && allowEmpty {
		var probe [1]byte
		count, readErr := result.Body.Read(probe[:])
		if count != 0 || (readErr != nil && readErr != io.EOF) {
			return 0, fmt.Errorf("%w: nonempty 204", ErrServerProtocol)
		}
		return result.StatusCode, nil
	}
	if result.StatusCode != http.StatusOK {
		return result.StatusCode, fmt.Errorf("%w: server returned HTTP %d", ErrServerProtocol, result.StatusCode)
	}
	if response == nil {
		return result.StatusCode, fmt.Errorf("%w: missing response target", ErrServerProtocol)
	}
	if result.ContentLength > maxHTTPResponse {
		return 0, fmt.Errorf("%w: oversized response", ErrServerProtocol)
	}
	if err := protocol.DecodeStrict(result.Body, response); err != nil {
		return 0, fmt.Errorf("%w: %v", ErrServerProtocol, err)
	}
	return result.StatusCode, nil
}

func (agent *Agent) accept(ctx context.Context, assignment protocol.Assignment, endpoint string, body any) error {
	var response protocol.AcceptResponse
	if _, err := agent.post(ctx, endpoint, body, &response, false); err != nil {
		return err
	}
	if err := response.ValidateFor(assignment); err != nil {
		return fmt.Errorf("%w: stale or negative acknowledgment", ErrServerProtocol)
	}
	return nil
}

func (agent *Agent) heartbeat(ctx context.Context, assignment protocol.Assignment, cancel context.CancelFunc) error {
	ticker := time.NewTicker(heartbeatInterval)
	defer ticker.Stop()
	for {
		select {
		case <-ctx.Done():
			return nil
		case <-ticker.C:
			facts, err := HostFacts()
			if err != nil {
				cancel()
				return err
			}
			body := protocol.Heartbeat{SchemaVersion: protocol.Version, AssignmentID: assignment.AssignmentID,
				AttemptID: assignment.AttemptID, AssignmentVersion: assignment.AssignmentVersion,
				FencingToken: assignment.FencingToken, Phase: "executing", HostFacts: facts}
			var response protocol.HeartbeatResponse
			if _, err := agent.post(ctx, "/v1/agents/me/heartbeat", body, &response, false); err != nil {
				// A task deadline or the local completion path cancels an in-flight
				// heartbeat. That expected cancellation must not suppress the
				// independent terminal evidence upload. A heartbeat's own timeout
				// remains an error while the task context is still live.
				if errors.Is(err, context.Canceled) || errors.Is(ctx.Err(), context.Canceled) ||
					errors.Is(ctx.Err(), context.DeadlineExceeded) {
					return nil
				}
				cancel()
				return err
			}
			if err := response.ValidateFor(assignment); err != nil {
				cancel()
				return err
			}
			if response.Cancel || response.Yield {
				cancel()
				return nil
			}
		}
	}
}

type logUploader struct {
	agent      *Agent
	ctx        context.Context
	assignment protocol.Assignment
	mu         sync.Mutex
	sequence   int64
	err        error
	pending    *protocol.LogChunk
}

type streamWriter struct {
	uploader *logUploader
	stream   string
}

func (writer *streamWriter) Write(data []byte) (int, error) {
	return writer.uploader.write(writer.stream, data)
}

func (uploader *logUploader) write(stream string, data []byte) (int, error) {
	uploader.mu.Lock()
	defer uploader.mu.Unlock()
	if uploader.err != nil {
		return 0, uploader.err
	}
	written := 0
	for len(data) > 0 {
		length := len(data)
		if length > protocol.MaxLogBytes {
			length = protocol.MaxLogBytes
		}
		part := data[:length]
		digest := sha256.Sum256(part)
		chunk := protocol.LogChunk{SchemaVersion: protocol.Version,
			AssignmentID: uploader.assignment.AssignmentID, AttemptID: uploader.assignment.AttemptID,
			AssignmentVersion: uploader.assignment.AssignmentVersion, FencingToken: uploader.assignment.FencingToken,
			Sequence: uploader.sequence + 1, Stream: stream,
			DataBase64: base64.StdEncoding.EncodeToString(part), SHA256: hex.EncodeToString(digest[:])}
		if err := chunk.Validate(); err != nil {
			uploader.err = err
			return written, err
		}
		if err := uploader.agent.accept(uploader.ctx, uploader.assignment, "/v1/attempts/"+uploader.assignment.AttemptID+"/logs", chunk); err != nil {
			uploader.err = err
			uploader.pending = &chunk
			return written, err
		}
		uploader.sequence++
		written += length
		data = data[length:]
	}
	return written, nil
}

func (uploader *logUploader) flushGrace(ctx context.Context) {
	uploader.mu.Lock()
	defer uploader.mu.Unlock()
	if uploader.pending == nil {
		return
	}
	if err := uploader.agent.accept(ctx, uploader.assignment,
		"/v1/attempts/"+uploader.assignment.AttemptID+"/logs", *uploader.pending); err == nil {
		uploader.sequence = uploader.pending.Sequence
		uploader.pending = nil
	}
}

func (uploader *logUploader) status() (int64, error) {
	uploader.mu.Lock()
	defer uploader.mu.Unlock()
	return uploader.sequence, uploader.err
}

func terminalReceipt(assignment protocol.Assignment, result executor.Result, start, end protocol.HostFacts, sequence int64, runErr, logErr error) protocol.TerminalReceipt {
	steps := make([]protocol.StepSummary, 0, len(result.Steps))
	for _, step := range result.Steps {
		steps = append(steps, protocol.StepSummary{Name: step.Name, StartedAt: step.StartedAt,
			EndedAt: step.EndedAt, DurationNS: int64(step.Duration), ExitCode: step.ExitCode,
			TimedOut: step.TimedOut, Cancelled: step.Cancelled, StdoutSHA256: step.StdoutSHA256,
			StderrSHA256: step.StderrSHA256, StdoutBytes: step.StdoutBytes, StderrBytes: step.StderrBytes})
	}
	outcome := "succeeded"
	switch {
	case result.TimedOut:
		outcome = "timed_out"
	case result.Cancelled:
		outcome = "cancelled"
	case runErr != nil || logErr != nil || result.ExitCode != 0:
		outcome = "failed"
	}
	if len(result.Steps) == 0 && outcome == "succeeded" {
		outcome = "failed"
	}
	var exit *int
	if !result.StartedAt.IsZero() && result.ExitCode >= 0 {
		value := result.ExitCode
		exit = &value
	}
	started := result.StartedAt
	ended := result.EndedAt
	if started.IsZero() {
		started = start.CapturedAt
	}
	if ended.IsZero() {
		ended = end.CapturedAt
	}
	if ended.Before(started) {
		ended = started
	}
	receipt := protocol.TerminalReceipt{SchemaVersion: protocol.Version,
		AssignmentID: assignment.AssignmentID, AttemptID: assignment.AttemptID,
		AssignmentVersion: assignment.AssignmentVersion, FencingToken: assignment.FencingToken,
		Outcome: outcome, ChildExitCode: exit, TimedOut: result.TimedOut, Cancelled: result.Cancelled,
		EvidenceComplete: runErr == nil && logErr == nil && len(result.Steps) > 0, StartedAt: started, EndedAt: ended,
		DurationNS: int64(result.Duration), Steps: steps, FinalLogSequence: sequence,
		CatalogSHA256: assignment.CatalogSHA256, SourceSnapshotSHA256: assignment.Source.SnapshotSHA256,
		HostFactsAtStart: start, HostFactsAtEnd: end}
	if runErr != nil {
		receipt.ErrorCode = "executor_error"
	} else if logErr != nil {
		receipt.ErrorCode = "log_upload_error"
	} else if len(result.Steps) == 0 {
		receipt.ErrorCode = "no_step_executed"
	}
	return receipt
}

// copy a bounded, untrusted HTTP body only through protocol.DecodeStrict.
var _ io.Writer = (*streamWriter)(nil)
