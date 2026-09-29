// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package server

import (
	"context"
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/json"
	"errors"
	"io"
	"math/big"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/protocol"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// The seven doors an agent knocks on, each with a body shaped for its own
// handler. Every refusal below happens before the store is consulted, which
// is what lets the whole table run against a plane that has no database: a
// door that started reading the store first would panic here rather than
// answer, and that is the point of the fixture.
const agentTrustedCommit = "0123456789abcdef0123456789abcdef01234567"

const knownAssignment = "01890a2b-7c3d-7e4f-8a1b-2c3d4e5f6a7b"

const knownAttempt = "01890a2b-7c3d-7e4f-8a1b-2c3d4e5f6a7c"

type agentDoor struct {
	target string
	// mismatched is a decodable body whose identity disagrees with the
	// path. Empty for the two doors that carry no path identity.
	mismatched string
	// detail is the wording the door uses for that disagreement.
	detail string
}

func agentDoors() map[string]agentDoor {
	return map[string]agentDoor{
		"claim": {target: "/v1/agents/me/claim"},
		"ack": {
			target:     "/v1/assignments/" + knownAssignment + "/ack",
			mismatched: `{"assignment_id":"` + knownAttempt + `"}`,
			detail:     "assignment identity mismatch",
		},
		"log": {
			target:     "/v1/attempts/" + knownAttempt + "/logs",
			mismatched: `{"attempt_id":"` + knownAssignment + `"}`,
			detail:     "attempt identity or log digest mismatch",
		},
		"result": {
			target:     "/v1/attempts/" + knownAttempt + "/result",
			mismatched: `{"attempt_id":"` + knownAssignment + `"}`,
			detail:     "invalid terminal receipt",
		},
		"artifact chunk": {
			target:     "/v1/attempts/" + knownAttempt + "/artifacts/chunk",
			mismatched: `{"attempt_id":"` + knownAssignment + `"}`,
			detail:     "attempt identity or artifact digest mismatch",
		},
		"artifact manifest": {
			target:     "/v1/attempts/" + knownAttempt + "/artifacts/manifest",
			mismatched: `{"attempt_id":"` + knownAssignment + `"}`,
			detail:     "attempt identity or artifact manifest mismatch",
		},
		"heartbeat": {target: "/v1/agents/me/heartbeat"},
	}
}

// agentPlane routes every agent door with a trusted commit already named, so
// the claim door is open rather than answering its configuration refusal.
// The store stays a zero value: nothing below may reach it.
func agentPlane(t *testing.T) http.Handler {
	t.Helper()
	cat, err := catalog.Load()
	if err != nil {
		t.Fatal(err)
	}
	api, err := NewWithOptions(&store.Store{}, cat, nil, agentTrustedCommit)
	if err != nil {
		t.Fatal(err)
	}
	return api.Handler()
}

// livePeer mints a client leaf inside its validity window, which is what
// verifiedAgentCertificate requires before a body is read. The certificate in
// denial_audit_test.go deliberately carries no window and is refused, so it
// cannot stand in here.
func livePeer(t *testing.T) *x509.Certificate {
	t.Helper()
	key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		t.Fatalf("generate key: %v", err)
	}
	template := &x509.Certificate{
		SerialNumber: big.NewInt(11),
		Subject:      pkix.Name{CommonName: "agent"},
		NotBefore:    time.Now().Add(-time.Hour),
		NotAfter:     time.Now().Add(time.Hour),
	}
	der, err := x509.CreateCertificate(rand.Reader, template, template, &key.PublicKey, key)
	if err != nil {
		t.Fatalf("create certificate: %v", err)
	}
	leaf, err := x509.ParseCertificate(der)
	if err != nil {
		t.Fatalf("parse certificate: %v", err)
	}
	return leaf
}

func knock(t *testing.T, handler http.Handler, target, contentType, body string, leaf *x509.Certificate) answered {
	t.Helper()
	request := httptest.NewRequest(http.MethodPost, target, strings.NewReader(body))
	if contentType != "" {
		request.Header.Set("Content-Type", contentType)
	}
	if leaf != nil {
		request.TLS = &tls.ConnectionState{
			PeerCertificates: []*x509.Certificate{leaf},
			VerifiedChains:   [][]*x509.Certificate{{leaf}},
		}
	}
	response := httptest.NewRecorder()
	handler.ServeHTTP(response, request)
	result := answered{status: response.Code, thread: response.Header().Get(correlationHeader)}
	if response.Body.Len() > 0 {
		if err := json.Unmarshal(response.Body.Bytes(), &result.body); err != nil {
			t.Fatalf("%s answered a body that is not JSON: %q", target, response.Body.String())
		}
	}
	return result
}

// TestEveryAgentDoorNamesTheOneMediaTypeItReads pins that the content type is
// judged ahead of the decoder on all seven doors. A door that read the body
// first would spend the JSON ceiling on a form post or an octet stream before
// discovering it could not have parsed it.
func TestEveryAgentDoorNamesTheOneMediaTypeItReads(t *testing.T) {
	handler := agentPlane(t)
	peer := livePeer(t)
	for _, contentType := range []string{"", "text/plain", "application/octet-stream", "text/json", "application/xml"} {
		for name, door := range agentDoors() {
			result := knock(t, handler, door.target, contentType, `{}`, peer)
			if result.status != http.StatusUnsupportedMediaType {
				t.Fatalf("%s with content type %q status = %d, want 415", name, contentType, result.status)
			}
			if result.body["code"] != "invalid_argument" || result.body["detail"] != "content type must be application/json" {
				t.Fatalf("%s with content type %q answered %+v", name, contentType, result.body)
			}
			if result.body["retryable"] != false {
				t.Fatalf("%s called an unreadable media type retryable", name)
			}
		}
	}
}

// And the charitable half of the same rule. The check is a prefix match, not
// an equality, so a client that names its charset is read. That also admits
// application/json-patch+json, which no door implements: pinned here as the
// looseness it is, so the next reader can see it was measured rather than
// assumed, and the body decoder is what turns the request away.
func TestAJSONContentTypePrefixIsRead(t *testing.T) {
	handler := agentPlane(t)
	peer := livePeer(t)
	for _, contentType := range []string{"application/json", "application/json; charset=utf-8", "application/json-patch+json"} {
		for name, door := range agentDoors() {
			result := knock(t, handler, door.target, contentType, `{"not_a_field":1}`, peer)
			if result.status != http.StatusBadRequest {
				t.Fatalf("%s with content type %q status = %d, want the decoder's 400", name, contentType, result.status)
			}
			if result.body["detail"] != "invalid agent request" {
				t.Fatalf("%s with content type %q answered %+v", name, contentType, result.body)
			}
		}
	}
}

// TestEveryAgentDoorRefusesADocumentItCannotDecode pins the decoder's own
// refusal on all seven doors and, with it, that decoding is strict: an
// unknown field and a second document after the first are both refused rather
// than quietly dropped. A silently ignored field is how a fencing token gets
// misspelled and never noticed.
func TestEveryAgentDoorRefusesADocumentItCannotDecode(t *testing.T) {
	handler := agentPlane(t)
	peer := livePeer(t)
	for _, body := range []string{
		``,
		`{`,
		`{"schema_version":`,
		`[]`,
		`"a string"`,
		`{"unknown_field":true}`,
		`{} {}`,
		`{"schema_version":1} trailing`,
	} {
		for name, door := range agentDoors() {
			result := knock(t, handler, door.target, "application/json", body, peer)
			if result.status != http.StatusBadRequest {
				t.Fatalf("%s given %q status = %d, want 400", name, body, result.status)
			}
			if result.body["code"] != "invalid_argument" || result.body["detail"] != "invalid agent request" {
				t.Fatalf("%s given %q answered %+v", name, body, result.body)
			}
		}
	}
}

// A JSON null is the one document that is not a decoder refusal: it decodes
// into a zero value and is then refused on the door's own terms. That is
// worth pinning rather than assuming, because the difference decides which
// wording an operator sees, and a door that ever accepted it would be
// accepting an empty grant.
func TestAJSONNullIsRefusedOnTheDoorsOwnTermsNotTheDecoders(t *testing.T) {
	handler := agentPlane(t)
	peer := livePeer(t)
	for name, expected := range map[string]struct {
		target string
		detail string
	}{
		"claim":     {target: "/v1/agents/me/claim", detail: "invalid agent claim"},
		"heartbeat": {target: "/v1/agents/me/heartbeat", detail: "invalid agent heartbeat"},
		"ack":       {target: "/v1/assignments/" + knownAssignment + "/ack", detail: "assignment identity mismatch"},
	} {
		result := knock(t, handler, expected.target, "application/json", `null`, peer)
		if result.status != http.StatusBadRequest || result.body["detail"] != expected.detail {
			t.Fatalf("%s given null answered %d %+v", name, result.status, result.body)
		}
	}
}

// TestAnAgentDoorRefusesAnIdentityThatDisagreesWithItsPath pins the five doors
// that carry an identifier in both the path and the body. The path is the one
// the caller was routed by; a body naming a different attempt is refused
// before the store, so a confused agent can never write its report onto
// someone else's attempt.
func TestAnAgentDoorRefusesAnIdentityThatDisagreesWithItsPath(t *testing.T) {
	handler := agentPlane(t)
	peer := livePeer(t)
	for name, door := range agentDoors() {
		if door.mismatched == "" {
			continue
		}
		result := knock(t, handler, door.target, "application/json", door.mismatched, peer)
		if result.status != http.StatusBadRequest {
			t.Fatalf("%s with a disagreeing identity status = %d, want 400", name, result.status)
		}
		if result.body["code"] != "invalid_argument" || result.body["detail"] != door.detail {
			t.Fatalf("%s answered %+v, want detail %q", name, result.body, door.detail)
		}
	}
}

// An absent identity is the same refusal as a disagreeing one. A body that
// simply omits the field must not be read as agreeing with whatever the path
// said.
func TestAnAbsentIdentityIsRefusedLikeADisagreeingOne(t *testing.T) {
	handler := agentPlane(t)
	peer := livePeer(t)
	for name, door := range agentDoors() {
		if door.mismatched == "" {
			continue
		}
		result := knock(t, handler, door.target, "application/json", `{"schema_version":`+itoa(protocol.Version)+`}`, peer)
		if result.status != http.StatusBadRequest || result.body["detail"] != door.detail {
			t.Fatalf("%s with no identity at all answered %d %+v", name, result.status, result.body)
		}
	}
}

func itoa(value int) string {
	if value == 0 {
		return "0"
	}
	digits := ""
	for value > 0 {
		digits = string(rune('0'+value%10)) + digits
		value /= 10
	}
	return digits
}

// TestTheTwoDoorsWithoutAPathIdentityAreJudgedOnTheirOwnTerms pins claim and
// heartbeat, which carry no identifier in the path and so are judged entirely
// by their own validation. Both refusals are worded for the door, which is
// what tells an operator reading a log which call was malformed.
func TestTheTwoDoorsWithoutAPathIdentityAreJudgedOnTheirOwnTerms(t *testing.T) {
	handler := agentPlane(t)
	peer := livePeer(t)
	version := itoa(protocol.Version)
	for name, judged := range map[string]struct {
		target string
		body   string
		detail string
	}{
		"a claim that would poll past the ceiling": {
			target: "/v1/agents/me/claim",
			body:   `{"schema_version":` + version + `,"poll_wait_ms":25001}`,
			detail: "invalid agent claim",
		},
		"a claim with no measured host": {
			target: "/v1/agents/me/claim",
			body:   `{"schema_version":` + version + `,"poll_wait_ms":0}`,
			detail: "invalid agent claim",
		},
		"a claim at the wrong schema version": {
			target: "/v1/agents/me/claim",
			body:   `{"schema_version":` + itoa(protocol.Version+1) + `}`,
			detail: "invalid agent claim",
		},
		"a heartbeat in no known phase": {
			target: "/v1/agents/me/heartbeat",
			body:   `{"schema_version":` + version + `,"assignment_id":"` + knownAssignment + `","attempt_id":"` + knownAttempt + `","assignment_version":1,"fencing_token":1,"phase":"thinking"}`,
			detail: "invalid agent heartbeat",
		},
		"a heartbeat with no grant behind it": {
			target: "/v1/agents/me/heartbeat",
			body:   `{"schema_version":` + version + `,"phase":"executing"}`,
			detail: "invalid agent heartbeat",
		},
	} {
		result := knock(t, handler, judged.target, "application/json", judged.body, peer)
		if result.status != http.StatusBadRequest {
			t.Fatalf("%s status = %d, want 400", name, result.status)
		}
		if result.body["code"] != "invalid_argument" || result.body["detail"] != judged.detail {
			t.Fatalf("%s answered %+v, want detail %q", name, result.body, judged.detail)
		}
	}
}

// unreadBody fails the test if anything reads it. An agent request refused on
// its transport identity must be refused before its body is touched.
type unreadBody struct{ t *testing.T }

func (b unreadBody) Read([]byte) (int, error) {
	b.t.Fatal("the body of a request from an unverified peer was read")
	return 0, io.EOF
}

func (unreadBody) Close() error { return nil }

// TestAnUnverifiedPeerIsRefusedBeforeItsBodyIsRead pins the order at the head
// of every agent handler: the transport identity is judged first, the denial
// is audited under the door's own action with the path's target, and the body
// never runs. JSON fields never become an identity here, so a request that
// claims an attempt in its body gets no credit for it.
func TestAnUnverifiedPeerIsRefusedBeforeItsBodyIsRead(t *testing.T) {
	for name, door := range map[string]struct {
		handle func(*Server, http.ResponseWriter, *http.Request)
		path   [2]string
		action string
		target string
	}{
		"claim":             {handle: (*Server).agentClaim, action: "agent.claim", target: "agent"},
		"ack":               {handle: (*Server).agentAck, path: [2]string{"assignment_id", knownAssignment}, action: "agent.ack", target: knownAssignment},
		"log":               {handle: (*Server).agentLog, path: [2]string{"attempt_id", knownAttempt}, action: "agent.log", target: knownAttempt},
		"result":            {handle: (*Server).agentResult, path: [2]string{"attempt_id", knownAttempt}, action: "agent.result", target: knownAttempt},
		"artifact chunk":    {handle: (*Server).agentArtifactChunk, path: [2]string{"attempt_id", knownAttempt}, action: "agent.artifact", target: knownAttempt},
		"artifact manifest": {handle: (*Server).agentArtifactManifest, path: [2]string{"attempt_id", knownAttempt}, action: "agent.artifact.manifest", target: knownAttempt},
		"heartbeat":         {handle: (*Server).agentHeartbeat, action: "agent.heartbeat", target: "agent"},
	} {
		auditor := &recordingAuditor{}
		api := &Server{audit: auditor, trustedAgentCommit: agentTrustedCommit}
		request := httptest.NewRequest(http.MethodPost, "/v1/agents", unreadBody{t: t})
		request.Header.Set("Content-Type", "application/json")
		if door.path[0] != "" {
			request.SetPathValue(door.path[0], door.path[1])
		}
		response := httptest.NewRecorder()
		door.handle(api, response, request)

		if response.Code != http.StatusNotFound {
			t.Fatalf("%s from an unverified peer status = %d, want 404", name, response.Code)
		}
		if len(auditor.records) != 1 {
			t.Fatalf("%s wrote %d denial records, want 1", name, len(auditor.records))
		}
		record := auditor.records[0]
		if record.actor != "unverified-peer" || record.action != door.action || record.target != door.target {
			t.Fatalf("%s audited %+v", name, record)
		}
	}
}

// TestAConflictIsAuditedUnderItsOwnActionBeforeItIsAnswered pins the middle
// arm of agentFailure. A lost race is not a denial, but it is still written to
// the audit trail, under the door's action with ".conflict" appended, so the
// two are distinguishable afterwards. The answer itself is the store's own
// 409.
func TestAConflictIsAuditedUnderItsOwnActionBeforeItIsAnswered(t *testing.T) {
	auditor := &recordingAuditor{}
	api := &Server{audit: auditor}
	response := httptest.NewRecorder()
	api.agentFailure(response, httptest.NewRequest(http.MethodPost, "/v1/attempts/x/result", nil),
		"agent.result", knownAttempt, store.ErrConflict)

	if response.Code != http.StatusConflict {
		t.Fatalf("conflict status = %d, want 409", response.Code)
	}
	if len(auditor.records) != 1 || auditor.records[0].action != "agent.result.conflict" ||
		auditor.records[0].target != knownAttempt {
		t.Fatalf("conflict audited %+v", auditor.records)
	}
}

// TestAConflictThatCannotBeAuditedIsNotAnsweredAsAConflict pins the fail-closed
// half. When the audit sink refuses, or there is no sink at all, the plane says
// it is unavailable and retryable rather than reporting a conflict it could not
// record. An unrecorded conflict answered as 409 would leave an agent retrying
// against a trail with no evidence of the race.
func TestAConflictThatCannotBeAuditedIsNotAnsweredAsAConflict(t *testing.T) {
	for name, api := range map[string]*Server{
		"the sink refuses":   {audit: &recordingAuditor{err: errors.New("audit sink is down")}},
		"there is no sink":   {},
		"the store is unset": {audit: nil, store: nil},
	} {
		response := httptest.NewRecorder()
		api.agentFailure(response, httptest.NewRequest(http.MethodPost, "/v1/attempts/x/logs", nil),
			"agent.log", knownAttempt, store.ErrConflict)

		if response.Code != http.StatusServiceUnavailable {
			t.Fatalf("%s: status = %d, want 503", name, response.Code)
		}
		var body map[string]any
		if err := json.Unmarshal(response.Body.Bytes(), &body); err != nil {
			t.Fatalf("%s: %v", name, err)
		}
		if body["detail"] != "conflict audit unavailable" || body["retryable"] != true {
			t.Fatalf("%s answered %+v", name, body)
		}
	}
}

// TestAFailureThatIsNeitherDeniedNorAConflictIsPassedStraightOn pins the last
// arm: everything else is the store's error to describe, and agentFailure adds
// nothing to it. The audit trail stays empty, because nothing was refused.
func TestAFailureThatIsNeitherDeniedNorAConflictIsPassedStraightOn(t *testing.T) {
	for name, expected := range map[string]struct {
		err    error
		status int
	}{
		"invalid":           {err: store.ErrInvalid, status: http.StatusBadRequest},
		"not found":         {err: store.ErrNotFound, status: http.StatusNotFound},
		"unavailable":       {err: errors.New("dial tcp: connection refused"), status: http.StatusServiceUnavailable},
		"the store is down": {err: store.ErrUnavailable, status: http.StatusServiceUnavailable},
	} {
		auditor := &recordingAuditor{}
		api := &Server{audit: auditor}
		response := httptest.NewRecorder()
		api.agentFailure(response, httptest.NewRequest(http.MethodPost, "/v1/attempts/x/logs", nil),
			"agent.log", knownAttempt, expected.err)

		if response.Code != expected.status {
			t.Fatalf("%s: status = %d, want %d", name, response.Code, expected.status)
		}
		if len(auditor.records) != 0 {
			t.Fatalf("%s was written to the denial trail: %+v", name, auditor.records)
		}
	}
}

// TestAnAcceptedAnswerRepeatsTheGrantItWasGiven pins the one shape every
// writing door answers with. The version and fence are echoed rather than
// minted, which is what lets an agent check that the plane accepted the grant
// it actually holds and not a newer one.
func TestAnAcceptedAnswerRepeatsTheGrantItWasGiven(t *testing.T) {
	answer := accepted(9, 4)
	if answer.SchemaVersion != protocol.Version {
		t.Fatalf("schema version = %d, want %d", answer.SchemaVersion, protocol.Version)
	}
	if answer.AssignmentVersion != 9 || answer.FencingToken != 4 || !answer.Accepted {
		t.Fatalf("accepted answer = %+v", answer)
	}
	if zero := accepted(0, 0); zero.Accepted != true || zero.AssignmentVersion != 0 {
		t.Fatalf("a zero grant was not echoed as given: %+v", zero)
	}
}

// TestADeniedAgentRequestNamesTheVerifiedPeerWhenThereIsOne pins the other
// half of the actor rule: a peer whose chain was verified is audited by the
// fingerprint of its certificate, which is the identifier api_principals holds.
// Only a request with no verified chain is "unverified-peer".
func TestADeniedAgentRequestNamesTheVerifiedPeerWhenThereIsOne(t *testing.T) {
	leaf := livePeer(t)
	auditor := &recordingAuditor{}
	api := &Server{audit: auditor}
	request := httptest.NewRequest(http.MethodPost, "/v1/agents/me/heartbeat", nil)
	request.TLS = &tls.ConnectionState{
		PeerCertificates: []*x509.Certificate{leaf},
		VerifiedChains:   [][]*x509.Certificate{{leaf}},
	}
	api.agentFailure(httptest.NewRecorder(), request, "agent.heartbeat", "agent", store.ErrDenied)

	want := "certificate-sha256:" + certificateFingerprint(leaf.Raw)
	if len(auditor.records) != 1 || auditor.records[0].actor != want {
		t.Fatalf("denial audited %+v, want actor %s", auditor.records, want)
	}
}

// TestADenialThatCannotBeRecordedIsNotAnsweredAsADenial pins deny's own
// fail-closed arm, the same rule as the conflict above: with no audit sink the
// plane reports unavailable rather than the 404 a denial would get.
func TestADenialThatCannotBeRecordedIsNotAnsweredAsADenial(t *testing.T) {
	api := &Server{}
	response := httptest.NewRecorder()
	api.agentFailure(response, httptest.NewRequest(http.MethodPost, "/v1/agents/me/claim", nil),
		"agent.claim", "agent", store.ErrDenied)

	if response.Code != http.StatusServiceUnavailable {
		t.Fatalf("status = %d, want 503", response.Code)
	}
	var body map[string]any
	if err := json.Unmarshal(response.Body.Bytes(), &body); err != nil {
		t.Fatal(err)
	}
	if body["detail"] != "authorization audit unavailable" {
		t.Fatalf("answered %+v", body)
	}
}

// TestAnUnavailableAuthorizationIsRetryableWhereADenialIsNot pins the split at
// the end of deny. Both are audited, but an authorization service that could
// not answer is a 503 an agent should retry, while a decision that it may not
// have the run is a 404 it should not. The retryable side is reached only by
// an error that is BOTH denied and unavailable: a bare unavailable is not a
// refusal at all and never enters deny, which the test below pins.
func TestAnUnavailableAuthorizationIsRetryableWhereADenialIsNot(t *testing.T) {
	for name, expected := range map[string]struct {
		err    error
		status int
		detail string
	}{
		"the authorizer could not answer": {err: errors.Join(store.ErrDenied, store.ErrUnavailable), status: http.StatusServiceUnavailable, detail: "authorization service unavailable"},
		"the authorizer said no":          {err: store.ErrDenied, status: http.StatusNotFound, detail: "run not found or access denied"},
	} {
		auditor := &recordingAuditor{}
		api := &Server{audit: auditor}
		response := httptest.NewRecorder()
		api.agentFailure(response, httptest.NewRequest(http.MethodPost, "/v1/agents/me/claim", nil),
			"agent.claim", "agent", expected.err)

		if response.Code != expected.status {
			t.Fatalf("%s: status = %d, want %d", name, response.Code, expected.status)
		}
		var body map[string]any
		if err := json.Unmarshal(response.Body.Bytes(), &body); err != nil {
			t.Fatalf("%s: %v", name, err)
		}
		if body["detail"] != expected.detail {
			t.Fatalf("%s answered %+v", name, body)
		}
		if len(auditor.records) != 1 {
			t.Fatalf("%s wrote %d denial records, want 1", name, len(auditor.records))
		}
	}
}

var _ = context.Background
