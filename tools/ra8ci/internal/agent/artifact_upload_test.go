// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package agent

import (
	"encoding/json"
	"errors"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/protocol"
)

// artifactRecorder answers both artifact endpoints the way the plane does and
// keeps what arrived, in arrival order.
type artifactRecorder struct {
	chunks    []protocol.ArtifactChunk
	manifests []protocol.ArtifactManifest
	failOn    string
}

func (recorder *artifactRecorder) handler(assignment protocol.Assignment) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		if recorder.failOn != "" && strings.HasSuffix(r.URL.Path, recorder.failOn) {
			w.WriteHeader(http.StatusConflict)
			return
		}
		switch {
		case strings.HasSuffix(r.URL.Path, "/artifacts/chunk"):
			var chunk protocol.ArtifactChunk
			if err := json.NewDecoder(r.Body).Decode(&chunk); err != nil || chunk.Validate() != nil {
				w.WriteHeader(http.StatusBadRequest)
				return
			}
			recorder.chunks = append(recorder.chunks, chunk)
			writeAccept(w, chunk.AssignmentVersion, chunk.FencingToken)
		case strings.HasSuffix(r.URL.Path, "/artifacts/manifest"):
			var manifest protocol.ArtifactManifest
			if err := json.NewDecoder(r.Body).Decode(&manifest); err != nil || manifest.Validate() != nil {
				w.WriteHeader(http.StatusBadRequest)
				return
			}
			recorder.manifests = append(recorder.manifests, manifest)
			writeAccept(w, manifest.AssignmentVersion, manifest.FencingToken)
		default:
			w.WriteHeader(http.StatusNotFound)
		}
	}
}

func writeAccept(w http.ResponseWriter, version, fence int64) {
	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(protocol.AcceptResponse{SchemaVersion: protocol.Version,
		AssignmentVersion: version, FencingToken: fence, Accepted: true})
}

func artifactCheckout(t *testing.T, files map[string]string) string {
	t.Helper()
	root := t.TempDir()
	for name, body := range files {
		path := filepath.Join(root, filepath.FromSlash(name))
		if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(path, []byte(body), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	return root
}

func TestArtifactEndpointsComeFromTheGrant(t *testing.T) {
	if got := artifactChunkPath(testAttemptID); got != "/v1/attempts/"+testAttemptID+"/artifacts/chunk" {
		t.Fatalf("chunk path = %q", got)
	}
	if got := artifactManifestPath(testAttemptID); got != "/v1/attempts/"+testAttemptID+"/artifacts/manifest" {
		t.Fatalf("manifest path = %q", got)
	}
}

func TestCollectStepArtifactsUploadsChunksThenManifests(t *testing.T) {
	assignment := testAssignment()
	recorder := &artifactRecorder{}
	agent, server := testAgent(t, recorder.handler(assignment))
	defer server.Close()
	agent.root = artifactCheckout(t, map[string]string{
		"out/report.xml": "<testsuite/>",
		"out/build.log":  strings.Repeat("log line\n", 64),
	})
	uploader, err := newArtifactUploader(agent, assignment)
	if err != nil {
		t.Fatal(err)
	}
	collector, err := NewArtifactCollector(agent.root, assignment, uploader.send,
		func() time.Time { return time.Unix(1790000000, 0).UTC() })
	if err != nil {
		t.Fatal(err)
	}
	agent.collectStepArtifacts(t.Context(), uploader, collector, "build",
		[]string{"out/report.xml", "out/build.log", "out/never-produced.txt"})
	manifests, err := uploader.status()
	if err != nil {
		t.Fatalf("collection failed: %v", err)
	}
	if len(manifests) != 2 || len(recorder.manifests) != 2 {
		t.Fatalf("manifests uploaded = %d, recorded = %d", len(manifests), len(recorder.manifests))
	}
	if len(recorder.chunks) != 2 {
		t.Fatalf("chunks recorded = %d", len(recorder.chunks))
	}
	// Every chunk of an artifact reaches the plane before the manifest that
	// closes it, which is what lets the plane refuse a close it cannot back.
	for index, manifest := range recorder.manifests {
		if recorder.chunks[index].Path != manifest.Path {
			t.Fatalf("manifest %d closes %q after chunk for %q", index,
				manifest.Path, recorder.chunks[index].Path)
		}
		if manifest.StepName != "build" || manifest.AttemptID != assignment.AttemptID ||
			manifest.FencingToken != assignment.FencingToken {
			t.Fatalf("manifest %d left the grant: %+v", index, manifest)
		}
	}
	// A declared output that was never produced is skipped, not reported.
	for _, manifest := range recorder.manifests {
		if manifest.Path == "out/never-produced.txt" {
			t.Fatalf("closed an artifact that does not exist")
		}
	}
}

func TestCollectStepArtifactsRecordsTheFirstFailure(t *testing.T) {
	assignment := testAssignment()
	recorder := &artifactRecorder{failOn: "/artifacts/manifest"}
	agent, server := testAgent(t, recorder.handler(assignment))
	defer server.Close()
	agent.root = artifactCheckout(t, map[string]string{"out/report.xml": "<testsuite/>"})
	uploader, err := newArtifactUploader(agent, assignment)
	if err != nil {
		t.Fatal(err)
	}
	collector, err := NewArtifactCollector(agent.root, assignment, uploader.send, time.Now)
	if err != nil {
		t.Fatal(err)
	}
	agent.collectStepArtifacts(t.Context(), uploader, collector, "build", []string{"out/report.xml"})
	manifests, err := uploader.status()
	if err == nil {
		t.Fatalf("a refused close was reported as delivered")
	}
	if len(manifests) != 0 {
		t.Fatalf("uploaded set names %d artifacts the plane refused", len(manifests))
	}
	// The first failure is the one kept, so the reported cause is the cause.
	first := err
	uploader.record(errors.New("later and less useful"))
	if _, again := uploader.status(); again != first {
		t.Fatalf("a later failure replaced the first")
	}
}

func TestArtifactUploaderRefusesAnotherGrant(t *testing.T) {
	assignment := testAssignment()
	recorder := &artifactRecorder{}
	agent, server := testAgent(t, recorder.handler(assignment))
	defer server.Close()
	uploader, err := newArtifactUploader(agent, assignment)
	if err != nil {
		t.Fatal(err)
	}
	stale := assignment
	stale.FencingToken = assignment.FencingToken + 1
	other, err := NewArtifactCollector(artifactCheckout(t, map[string]string{"out/report.xml": "x"}),
		stale, uploader.send, time.Now)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := other.Collect(t.Context(), "build", []string{"out/report.xml"}); !errors.Is(err, ErrUnsafeArtifact) {
		t.Fatalf("stale fence accepted: %v", err)
	}
	if len(recorder.chunks) != 0 {
		t.Fatalf("a chunk fenced to another grant reached the plane")
	}
}

func TestNewArtifactUploaderRefusesPartialWiring(t *testing.T) {
	if _, err := newArtifactUploader(nil, testAssignment()); !errors.Is(err, ErrUnsafeAssignment) {
		t.Fatalf("agentless uploader accepted: %v", err)
	}
	agent, server := testAgent(t, func(http.ResponseWriter, *http.Request) {})
	defer server.Close()
	if _, err := newArtifactUploader(agent, protocol.Assignment{}); err == nil {
		t.Fatalf("uploader accepted an invalid grant")
	}
}

// The v1 catalog gate refuses any task that declares outputs, so nothing the
// agent can be assigned today produces an artifact. This pins that, so the
// slice that lifts the gate is the slice that wires collection into execute
// rather than discovering the dead end later.
func TestNoReviewedTaskDeclaresOutputsYet(t *testing.T) {
	definitions, err := catalog.Load()
	if err != nil {
		t.Fatal(err)
	}
	for _, name := range definitions.Names() {
		task, found := definitions.Task(name)
		if !found {
			t.Fatalf("catalog names %q but does not hold it", name)
		}
		if len(task.Outputs) != 0 {
			t.Fatalf("task %q declares outputs, so execute must now collect them", name)
		}
	}
}
