// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package agent

import (
	"context"
	"errors"
	"os"
	"path/filepath"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/executor"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/protocol"
)

func artifactClock() func() time.Time {
	return func() time.Time { return time.Date(2026, 9, 24, 20, 0, 0, 0, time.UTC) }
}

// attemptFixture wires an agent at a checkout holding one produced output,
// plus the recorder standing in for the plane's two artifact endpoints.
func attemptFixture(t *testing.T, recorder *artifactRecorder, body string) (*Agent, protocol.Assignment, func()) {
	t.Helper()
	assignment := testAssignment()
	agent, server := testAgent(t, recorder.handler(assignment))
	root := t.TempDir()
	if err := os.MkdirAll(filepath.Join(root, "out"), 0o755); err != nil {
		server.Close()
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(root, "out", "report.txt"), []byte(body), 0o644); err != nil {
		server.Close()
		t.Fatal(err)
	}
	agent.root = root
	return agent, assignment, server.Close
}

func producedResult(step string) executor.Result {
	return executor.Result{TaskName: "format-check", Steps: []executor.StepResult{
		{Name: "prepare"}, {Name: step},
	}}
}

func outputTask() catalog.Task {
	return catalog.Task{Name: "format-check", Version: 1, Outputs: []string{"out/report.txt"}}
}

func TestArtifactStepNameAttributesToTheLastStepThatRan(t *testing.T) {
	if name := artifactStepName(producedResult("compile")); name != "compile" {
		t.Fatalf("step name = %q, want compile", name)
	}
	if name := artifactStepName(executor.Result{}); name != "" {
		t.Fatalf("step name for an attempt that ran nothing = %q, want empty", name)
	}
	if name := artifactStepName(executor.Result{Steps: []executor.StepResult{{Name: ""}}}); name != "" {
		t.Fatalf("nameless step = %q, want empty", name)
	}
}

func TestArtifactsWantedRefusesCancelledAndUndeclared(t *testing.T) {
	declared := []string{"out/report.txt"}
	if !artifactsWanted(producedResult("compile"), declared) {
		t.Fatal("a completed attempt with declared outputs must collect")
	}
	timedOut := producedResult("compile")
	timedOut.TimedOut = true
	if !artifactsWanted(timedOut, declared) {
		t.Fatal("a deadline must still collect: the outputs explain it")
	}
	cancelled := producedResult("compile")
	cancelled.Cancelled = true
	if artifactsWanted(cancelled, declared) {
		t.Fatal("a cancelled attempt must not hold the guest uploading")
	}
	if artifactsWanted(producedResult("compile"), nil) {
		t.Fatal("no declared outputs must collect nothing")
	}
	if artifactsWanted(executor.Result{}, declared) {
		t.Fatal("an attempt that ran no step produced nothing")
	}
}

func TestCollectAttemptArtifactsUploadsDeclaredOutput(t *testing.T) {
	recorder := &artifactRecorder{}
	agent, assignment, closeServer := attemptFixture(t, recorder, "produced evidence\n")
	defer closeServer()
	manifests, err := agent.collectAttemptArtifacts(context.Background(), assignment,
		outputTask(), producedResult("compile"), artifactClock())
	if err != nil {
		t.Fatalf("collect = %v", err)
	}
	if len(manifests) != 1 || len(recorder.chunks) != 1 || len(recorder.manifests) != 1 {
		t.Fatalf("uploaded %d manifests, plane saw %d chunks and %d manifests",
			len(manifests), len(recorder.chunks), len(recorder.manifests))
	}
	chunk := recorder.chunks[0]
	manifest := recorder.manifests[0]
	if chunk.StepName != "compile" || chunk.Path != "out/report.txt" || chunk.AttemptID != assignment.AttemptID {
		t.Fatalf("chunk is not fenced to this attempt and step: %+v", chunk)
	}
	if err := manifest.Covers(chunk); err != nil {
		t.Fatalf("manifest does not cover its own chunk: %v", err)
	}
	if manifest.TotalBytes != int64(len("produced evidence\n")) || manifest.Truncated {
		t.Fatalf("manifest misstates the bytes uploaded: %+v", manifest)
	}
}

func TestCollectAttemptArtifactsSpendsNothingWhenUnwanted(t *testing.T) {
	for _, unwanted := range []struct {
		name   string
		task   catalog.Task
		result executor.Result
	}{
		{"no declared outputs", catalog.Task{Name: "format-check", Version: 1}, producedResult("compile")},
		{"cancelled", outputTask(), func() executor.Result {
			result := producedResult("compile")
			result.Cancelled = true
			return result
		}()},
		{"no step ran", outputTask(), executor.Result{}},
	} {
		t.Run(unwanted.name, func(t *testing.T) {
			recorder := &artifactRecorder{}
			agent, assignment, closeServer := attemptFixture(t, recorder, "produced evidence\n")
			defer closeServer()
			manifests, err := agent.collectAttemptArtifacts(context.Background(), assignment,
				unwanted.task, unwanted.result, artifactClock())
			if err != nil || len(manifests) != 0 {
				t.Fatalf("collect = %v, %v", manifests, err)
			}
			if len(recorder.chunks) != 0 || len(recorder.manifests) != 0 {
				t.Fatalf("spent an upload on an attempt that wanted none: %d chunks, %d manifests",
					len(recorder.chunks), len(recorder.manifests))
			}
		})
	}
}

func TestCollectAttemptArtifactsReportsAnUploadFailure(t *testing.T) {
	recorder := &artifactRecorder{failOn: "/artifacts/chunk"}
	agent, assignment, closeServer := attemptFixture(t, recorder, "produced evidence\n")
	defer closeServer()
	manifests, err := agent.collectAttemptArtifacts(context.Background(), assignment,
		outputTask(), producedResult("compile"), artifactClock())
	if err == nil {
		t.Fatal("a refused chunk must be reported, not swallowed")
	}
	if len(manifests) != 0 || len(recorder.manifests) != 0 {
		t.Fatalf("closed an artifact the plane never received: %v, %v", manifests, recorder.manifests)
	}
}

func TestCollectAttemptArtifactsRefusesPartialWiring(t *testing.T) {
	recorder := &artifactRecorder{}
	agent, assignment, closeServer := attemptFixture(t, recorder, "produced evidence\n")
	defer closeServer()
	if _, err := agent.collectAttemptArtifacts(context.Background(), assignment,
		outputTask(), producedResult("compile"), nil); !errors.Is(err, ErrUnsafeArtifact) {
		t.Fatalf("clockless collection = %v, want ErrUnsafeArtifact", err)
	}
	var missing *Agent
	if _, err := missing.collectAttemptArtifacts(context.Background(), assignment,
		outputTask(), producedResult("compile"), artifactClock()); !errors.Is(err, ErrUnsafeAssignment) {
		t.Fatalf("agentless collection = %v, want ErrUnsafeAssignment", err)
	}
}

func TestTerminalReceiptCarriesArtifactEvidenceFailure(t *testing.T) {
	assignment := testAssignment()
	facts := protocol.HostFacts{Cores: 2, RAMBytes: 1 << 30, RAMFreeBytes: 1 << 29,
		Load1: 0.5, LoadKind: "unix", OS: "linux", Arch: "amd64", CapturedAt: time.Now().UTC()}
	result := executor.Result{TaskName: "format-check", StartedAt: time.Now().UTC().Add(-time.Second),
		EndedAt: time.Now().UTC(), Steps: []executor.StepResult{{Name: "compile"}}}
	receipt := terminalReceipt(assignment, result, facts, facts, 2, nil, nil, errors.New("artifact refused"))
	if receipt.Outcome != "failed" || receipt.EvidenceComplete || receipt.ErrorCode != "artifact_upload_error" {
		t.Fatalf("artifact failure is not on the receipt: %+v", receipt)
	}
	// A log failure outranks it: the logs are the attempt's primary evidence.
	receipt = terminalReceipt(assignment, result, facts, facts, 2, nil,
		errors.New("broken logs"), errors.New("artifact refused"))
	if receipt.ErrorCode != "log_upload_error" {
		t.Fatalf("error code = %q, want log_upload_error", receipt.ErrorCode)
	}
	// And a clean attempt still succeeds with complete evidence.
	receipt = terminalReceipt(assignment, result, facts, facts, 2, nil, nil, nil)
	if receipt.Outcome != "succeeded" || !receipt.EvidenceComplete || receipt.ErrorCode != "" {
		t.Fatalf("clean attempt = %+v", receipt)
	}
}
