// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package agent

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"net/http"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/protocol"
)

// Artifacts are evidence about a run that already happened, so the uploader
// is the one part of an attempt that must never take the attempt down with
// it. What it refuses, it refuses before anything reaches the wire; what it
// cannot do, it records rather than returns. These are the doors it answers
// without a plane behind it at all.

// manifestFor builds a manifest the protocol accepts, fenced to the grant
// given, so a refusal under test is the uploader's own and not the
// protocol's.
func manifestFor(assignment protocol.Assignment, path string) protocol.ArtifactManifest {
	body := []byte("one closed artifact\n")
	digest := sha256.Sum256(body)
	return protocol.ArtifactManifest{SchemaVersion: protocol.Version,
		AssignmentID: assignment.AssignmentID, AttemptID: assignment.AttemptID,
		AssignmentVersion: assignment.AssignmentVersion, FencingToken: assignment.FencingToken,
		StepName: "format-check", Path: path, TotalBytes: int64(len(body)),
		SHA256: hex.EncodeToString(digest[:]), FinalSequence: 1,
		CapturedAt: time.Now().UTC()}
}

// An uploader that was never wired, or was wired without an agent, answers
// both of its doors rather than dereferencing its way to a panic. A nil
// method receiver is reachable here because the uploader is handed around as
// a pointer and a failed construction returns nil beside its error.
func TestAnUploaderWithNoAgentBehindItRefusesBothDoors(t *testing.T) {
	assignment := testAssignment()
	chunk := retryTestChunk(assignment)
	manifests := []protocol.ArtifactManifest{manifestFor(assignment, "build/report.txt")}

	for _, absent := range []struct {
		named    string
		uploader *artifactUploader
	}{
		{"no uploader at all", nil},
		{"an uploader with no agent", &artifactUploader{assignment: assignment}},
	} {
		t.Run(absent.named, func(t *testing.T) {
			if err := absent.uploader.send(context.Background(), chunk); !errors.Is(err, ErrUnsafeAssignment) {
				t.Fatalf("send err = %v, want ErrUnsafeAssignment", err)
			}
			if err := absent.uploader.Close(context.Background(), manifests); !errors.Is(err, ErrUnsafeAssignment) {
				t.Fatalf("close err = %v, want ErrUnsafeAssignment", err)
			}
		})
	}
}

// The chunk is validated again at the uploader rather than trusted from the
// collector, because this is the last place before the wire. An invalid chunk
// is refused as unsafe artifact evidence and never spends a request.
func TestAnInvalidChunkIsRefusedBeforeItReachesThePlane(t *testing.T) {
	assignment := testAssignment()
	agent, server := testAgent(t, func(w http.ResponseWriter, r *http.Request) {
		t.Errorf("the plane was asked for %s", r.URL.Path)
	})
	defer server.Close()
	uploader, err := newArtifactUploader(agent, assignment)
	if err != nil {
		t.Fatalf("newArtifactUploader: %v", err)
	}

	broken := retryTestChunk(assignment)
	broken.SHA256 = ""
	if err := uploader.send(context.Background(), broken); !errors.Is(err, ErrUnsafeArtifact) {
		t.Fatalf("send err = %v, want ErrUnsafeArtifact", err)
	}
}

// A close is validated as a SET, so one bad member refuses the whole close:
// the byte total and the path count are budgets of one attempt, and a set
// that does not hold together answers them against the wrong denominator.
func TestACloseThatDoesNotHoldTogetherIsRefused(t *testing.T) {
	assignment := testAssignment()
	agent, server := testAgent(t, func(w http.ResponseWriter, r *http.Request) {
		t.Errorf("the plane was asked for %s", r.URL.Path)
	})
	defer server.Close()
	uploader, err := newArtifactUploader(agent, assignment)
	if err != nil {
		t.Fatalf("newArtifactUploader: %v", err)
	}

	good := manifestFor(assignment, "build/report.txt")
	for _, refused := range []struct {
		named     string
		manifests []protocol.ArtifactManifest
	}{
		{"a member the protocol refuses", []protocol.ArtifactManifest{
			func() protocol.ArtifactManifest {
				broken := manifestFor(assignment, "build/report.txt")
				broken.TotalBytes = 0
				return broken
			}()}},
		{"the same path twice", []protocol.ArtifactManifest{good, good}},
		{"a good member beside a bad one", []protocol.ArtifactManifest{good,
			func() protocol.ArtifactManifest {
				broken := manifestFor(assignment, "build/other.txt")
				broken.SHA256 = "not a digest"
				return broken
			}()}},
	} {
		t.Run(refused.named, func(t *testing.T) {
			if err := uploader.Close(context.Background(), refused.manifests); !errors.Is(err, ErrUnsafeArtifact) {
				t.Fatalf("close err = %v, want ErrUnsafeArtifact", err)
			}
			if held, err := uploader.status(); len(held) != 0 || err != nil {
				t.Fatalf("status = %v, %v, want nothing held and no recorded failure", held, err)
			}
		})
	}
}

// A close fenced to a different grant is refused at the uploader, so a stale
// collector cannot write over a later attempt's evidence. The refusal is per
// member and stops the close: a set whose first member is ours and whose
// second is fenced elsewhere uploads neither, because the whole set is judged
// before any of it is offered.
func TestACloseFencedToAnotherGrantUploadsNothing(t *testing.T) {
	assignment := testAssignment()
	agent, server := testAgent(t, func(w http.ResponseWriter, r *http.Request) {
		t.Errorf("the plane was asked for %s", r.URL.Path)
	})
	defer server.Close()
	uploader, err := newArtifactUploader(agent, assignment)
	if err != nil {
		t.Fatalf("newArtifactUploader: %v", err)
	}

	later := assignment
	later.FencingToken = assignment.FencingToken + 1
	stale := manifestFor(later, "build/report.txt")
	if err := uploader.Close(context.Background(), []protocol.ArtifactManifest{stale}); !errors.Is(err, ErrUnsafeArtifact) {
		t.Fatalf("close err = %v, want ErrUnsafeArtifact", err)
	}
	if held, err := uploader.status(); len(held) != 0 || err != nil {
		t.Fatalf("status = %v, %v, want nothing uploaded", held, err)
	}
}

// Collection is best effort by design, so the step collector is a no-op in
// each of the three ways it can be handed nothing to do, and never a
// failure recorded against the attempt's evidence.
func TestCollectingWithNothingToCollectIsANoOp(t *testing.T) {
	assignment := testAssignment()
	agent, server := testAgent(t, func(w http.ResponseWriter, r *http.Request) {
		t.Errorf("the plane was asked for %s", r.URL.Path)
	})
	defer server.Close()
	uploader, err := newArtifactUploader(agent, assignment)
	if err != nil {
		t.Fatalf("newArtifactUploader: %v", err)
	}

	agent.collectStepArtifacts(context.Background(), nil, nil, "format-check", []string{"build/report.txt"})
	agent.collectStepArtifacts(context.Background(), uploader, nil, "format-check", []string{"build/report.txt"})
	agent.collectStepArtifacts(context.Background(), uploader, &ArtifactCollector{}, "format-check", nil)

	if held, err := uploader.status(); len(held) != 0 || err != nil {
		t.Fatalf("status = %v, %v, want nothing held and no recorded failure", held, err)
	}
}

// status is read on the way out of an attempt, including one that failed
// before an uploader existed, so it answers on a nil uploader as nothing
// held and nothing wrong.
func TestTheStatusOfAnUploaderThatNeverExistedIsEmpty(t *testing.T) {
	var uploader *artifactUploader
	held, err := uploader.status()
	if held != nil || err != nil {
		t.Fatalf("status = %v, %v, want nothing", held, err)
	}
}

// The first failure is the one kept: a later one would tell an operator about
// the consequence rather than the cause.
func TestOnlyTheFirstCollectionFailureIsKept(t *testing.T) {
	uploader := &artifactUploader{}
	first := errors.New("the cause")
	uploader.record(nil)
	uploader.record(first)
	uploader.record(errors.New("the consequence"))

	if _, err := uploader.status(); !errors.Is(err, first) {
		t.Fatalf("status err = %v, want the first failure", err)
	}
}
