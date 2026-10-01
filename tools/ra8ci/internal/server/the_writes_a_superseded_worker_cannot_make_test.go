//go:build integration

package server

import (
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"net/http"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/protocol"
)

// What the agent write doors do once an attempt is really running.
//
// Every one of these doors takes a grant and writes evidence under it: a log
// chunk, an artifact chunk, the manifest that closes an artifact. The door
// checks the body's shape, then hands it to the store, and the store is the
// only thing that knows whether the grant is still live. So the refusal that
// matters is not a malformed document, which the_doors_an_agent_knocks_on_test.go
// already holds, but a well-formed document from a worker that has been
// superseded. Those writes must not land: evidence written under a dead fence
// would be attributed to an attempt someone else now owns.

// uploaded is one chunk of bytes with the digest the doors will check it
// against, so a test never has to hand-compute a SHA256.
type uploaded struct {
	base64 string
	sha256 string
	bytes  int64
}

func uploadable(payload string) uploaded {
	sum := sha256.Sum256([]byte(payload))
	return uploaded{
		base64: base64.StdEncoding.EncodeToString([]byte(payload)),
		sha256: hex.EncodeToString(sum[:]),
		bytes:  int64(len(payload)),
	}
}

// running is a claimed and acknowledged attempt: the state every door below
// requires, since an unacknowledged attempt is refused as a conflict whatever
// it writes.
func running(t *testing.T) (agentWork, protocol.Assignment) {
	t.Helper()
	work := claimingPlane(t, true)
	assignment := work.claimOne(t)
	work.acknowledge(t, assignment)
	return work, assignment
}

func TestIntegrationALogChunkIsAcceptedOnItsOwnFenceAndRefusedOnAnother(t *testing.T) {
	work, assignment := running(t)
	payload := uploadable("configure: ok\n")
	path := "/v1/attempts/" + assignment.AttemptID + "/logs"

	chunk := protocol.LogChunk{
		SchemaVersion:     protocol.Version,
		AssignmentID:      assignment.AssignmentID,
		AttemptID:         assignment.AttemptID,
		AssignmentVersion: assignment.AssignmentVersion,
		FencingToken:      assignment.FencingToken,
		Sequence:          1,
		Stream:            "stdout",
		StepName:          "format-check",
		DataBase64:        payload.base64,
		SHA256:            payload.sha256,
	}

	accepted := work.agentKnock(t, path, chunk)
	if accepted.Code != http.StatusOK {
		t.Fatalf("a log chunk on a live grant answered %d: %s", accepted.Code, accepted.Body.String())
	}

	// The same chunk under a fence this attempt does not hold. It is a
	// perfectly well-formed document, so nothing before the store can refuse
	// it: this is the store's own judgement reaching the caller.
	superseded := chunk
	superseded.Sequence = 2
	superseded.FencingToken = assignment.FencingToken + 1
	refused := work.agentKnock(t, path, superseded)
	if refused.Code == http.StatusOK {
		t.Fatalf("a superseded worker wrote a log: %s", refused.Body.String())
	}
	if refused.Code < 400 {
		t.Fatalf("a superseded log write answered %d, want a refusal", refused.Code)
	}
}

func TestIntegrationAnArtifactIsUploadedAndClosedOnlyOnItsOwnFence(t *testing.T) {
	work, assignment := running(t)
	payload := uploadable("the artifact body")
	chunkPath := "/v1/attempts/" + assignment.AttemptID + "/artifacts/chunk"
	manifestPath := "/v1/attempts/" + assignment.AttemptID + "/artifacts/manifest"

	chunk := protocol.ArtifactChunk{
		SchemaVersion:     protocol.Version,
		AssignmentID:      assignment.AssignmentID,
		AttemptID:         assignment.AttemptID,
		AssignmentVersion: assignment.AssignmentVersion,
		FencingToken:      assignment.FencingToken,
		StepName:          "format-check",
		Path:              "reports/format.txt",
		Sequence:          1,
		Offset:            0,
		DataBase64:        payload.base64,
		SHA256:            payload.sha256,
	}
	if result := work.agentKnock(t, chunkPath, chunk); result.Code != http.StatusOK {
		t.Fatalf("an artifact chunk on a live grant answered %d: %s", result.Code, result.Body.String())
	}

	superseded := chunk
	superseded.FencingToken = assignment.FencingToken + 1
	if result := work.agentKnock(t, chunkPath, superseded); result.Code == http.StatusOK {
		t.Fatalf("a superseded worker uploaded an artifact chunk: %s", result.Body.String())
	}

	manifest := protocol.ArtifactManifest{
		SchemaVersion:     protocol.Version,
		AssignmentID:      assignment.AssignmentID,
		AttemptID:         assignment.AttemptID,
		AssignmentVersion: assignment.AssignmentVersion,
		FencingToken:      assignment.FencingToken,
		StepName:          "format-check",
		Path:              "reports/format.txt",
		TotalBytes:        payload.bytes,
		SHA256:            payload.sha256,
		FinalSequence:     1,
		CapturedAt:        time.Now().UTC(),
	}

	// A superseded close is refused for the same reason an upload is: the
	// manifest states what the plane should now hold, and a dead fence must
	// not get to say that.
	supersededClose := manifest
	supersededClose.FencingToken = assignment.FencingToken + 1
	if result := work.agentKnock(t, manifestPath, supersededClose); result.Code == http.StatusOK {
		t.Fatalf("a superseded worker closed an artifact: %s", result.Body.String())
	}

	closed := work.agentKnock(t, manifestPath, manifest)
	if closed.Code != http.StatusOK {
		t.Fatalf("closing an artifact on a live grant answered %d: %s", closed.Code, closed.Body.String())
	}

	// The door's own comment promises a retrying agent never has to decide
	// whether its previous close landed, so the second close is the same 200
	// rather than a conflict.
	again := work.agentKnock(t, manifestPath, manifest)
	if again.Code != http.StatusOK {
		t.Fatalf("a repeated close answered %d: %s", again.Code, again.Body.String())
	}
	if again.Body.String() != closed.Body.String() {
		t.Fatalf("a repeated close answered differently:\nfirst  %s\nsecond %s",
			closed.Body.String(), again.Body.String())
	}
}

func TestIntegrationAnAcknowledgementOnADeadFenceIsRefused(t *testing.T) {
	work := claimingPlane(t, true)
	assignment := work.claimOne(t)

	// Acknowledging is itself a write under the grant, so it is judged the
	// same way. A worker whose fence has moved cannot confirm the work and
	// start running against it.
	result := work.agentKnock(t, "/v1/assignments/"+assignment.AssignmentID+"/ack", protocol.Ack{
		SchemaVersion:        protocol.Version,
		AssignmentID:         assignment.AssignmentID,
		AttemptID:            assignment.AttemptID,
		AssignmentVersion:    assignment.AssignmentVersion,
		FencingToken:         assignment.FencingToken + 1,
		CatalogSHA256:        assignment.CatalogSHA256,
		SourceSnapshotSHA256: assignment.Source.SnapshotSHA256,
		HostFacts:            work.facts,
	})
	if result.Code == http.StatusOK {
		t.Fatalf("an acknowledgement on a dead fence was accepted: %s", result.Body.String())
	}
	if result.Code < 400 {
		t.Fatalf("a dead-fence acknowledgement answered %d, want a refusal", result.Code)
	}

	// And the real one still works afterwards: the refusal above rejected the
	// document, it did not poison the grant.
	work.acknowledge(t, assignment)
}
