package server

import (
	"bytes"
	"context"
	"crypto/tls"
	"errors"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

const (
	boardTestRequestID = "01996f90-3415-7cfe-8ff1-600058131afd"
	boardTestLeaseID   = "01996f90-3415-7cfe-8ff1-600058131afe"
	boardTestProofID   = "01996f90-3415-7cfe-8ff1-600058131aff"
)

type fakeBoardStore struct {
	authorizeErr error
	auditErr     error
	applyErr     error
	challengeErr error
	board        board.Snapshot
	command      board.Command
	neutral      *store.NeutralSubmission
	verifier     store.NeutralReceiptVerifier
	version      uint64
	peer         *tls.ConnectionState
	repo, id     string
	action       string
	reads        int
	applies      int
	audits       int
	challenges   int
}

func (f *fakeBoardStore) AuthorizeBoardPeer(_ context.Context, peer *tls.ConnectionState, repo, id string) (store.BoardActor, error) {
	f.peer, f.repo, f.id = peer, repo, id
	return store.BoardActor{}, f.authorizeErr
}

func (f *fakeBoardStore) GetBoard(_ context.Context, _ string) (board.Snapshot, error) {
	f.reads++
	return f.board, nil
}

func (f *fakeBoardStore) ApplyBoardCommand(_ context.Context, _ store.BoardActor, cmd board.Command, version uint64, neutral *store.NeutralSubmission, verifier store.NeutralReceiptVerifier, _ time.Time) (board.Snapshot, []board.Event, error) {
	f.applies++
	f.command, f.version, f.neutral, f.verifier = cmd, version, neutral, verifier
	return f.board, nil, f.applyErr
}

func (f *fakeBoardStore) IssueBoardNeutralChallenge(_ context.Context, _ store.BoardActor, version uint64, purpose string) (store.NeutralChallenge, error) {
	f.challenges++
	f.version = version
	return store.NeutralChallenge{ID: boardTestProofID, Purpose: purpose}, f.challengeErr
}

func (f *fakeBoardStore) AuditDenied(_ context.Context, _ string, action, _ string) error {
	f.audits++
	f.action = action
	return f.auditErr
}

type fakeNeutralVerifier struct{}

func (fakeNeutralVerifier) VerifyNeutralReceipt(context.Context, store.NeutralChallenge, []byte) error {
	return nil
}

func boardTestMux(t *testing.T, f *fakeBoardStore, verifier store.NeutralReceiptVerifier) *http.ServeMux {
	t.Helper()
	mux := http.NewServeMux()
	if err := RegisterBoardRoutes(mux, f, verifier, "bsikar/ra8-firmware"); err != nil {
		t.Fatal(err)
	}
	return mux
}

func boardTestRequest(method, path, body string) *http.Request {
	r := httptest.NewRequest(method, path, strings.NewReader(body))
	r.Header.Set("Content-Type", "application/json")
	r.TLS = &tls.ConnectionState{}
	return r
}

func TestBoardRegistrationRejectsMissingDependencies(t *testing.T) {
	if RegisterBoardRoutes(nil, &fakeBoardStore{}, nil, "repo") == nil {
		t.Fatal("nil mux was accepted")
	}
	if RegisterBoardRoutes(http.NewServeMux(), nil, nil, "repo") == nil {
		t.Fatal("nil store was accepted")
	}
	if RegisterBoardRoutes(http.NewServeMux(), &fakeBoardStore{}, nil, "") == nil {
		t.Fatal("missing repository was accepted")
	}
}

func TestBoardStatusRequiresCertScopedGrantAndAuditsDenial(t *testing.T) {
	f := &fakeBoardStore{authorizeErr: store.ErrDenied}
	w := httptest.NewRecorder()
	boardTestMux(t, f, nil).ServeHTTP(w, boardTestRequest("GET", "/v1/boards/ek-ra8d2", ""))
	if w.Code != http.StatusNotFound || f.audits != 1 || f.action != "board.status" || f.reads != 0 || f.repo != "bsikar/ra8-firmware" || f.id != "ek-ra8d2" || f.peer == nil {
		t.Fatalf("authorization denial leaked or bypassed: status=%d audit=%d action=%q read=%d repo=%q id=%q", w.Code, f.audits, f.action, f.reads, f.repo, f.id)
	}
	f.auditErr = store.ErrUnavailable
	w = httptest.NewRecorder()
	boardTestMux(t, f, nil).ServeHTTP(w, boardTestRequest("GET", "/v1/boards/ek-ra8d2", ""))
	if w.Code != http.StatusServiceUnavailable || f.reads != 0 {
		t.Fatalf("audit failure must fail closed: status=%d reads=%d", w.Code, f.reads)
	}
}

func TestBoardStatusReturnsSnapshotOnlyAfterAuthorization(t *testing.T) {
	f := &fakeBoardStore{board: board.Snapshot{BoardID: "ek-ra8d2", Phase: board.Ready}}
	w := httptest.NewRecorder()
	boardTestMux(t, f, nil).ServeHTTP(w, boardTestRequest("GET", "/v1/boards/ek-ra8d2", ""))
	if w.Code != http.StatusOK || f.reads != 1 || !strings.Contains(w.Body.String(), "ek-ra8d2") {
		t.Fatalf("authorized status failed: status=%d reads=%d body=%s", w.Code, f.reads, w.Body.String())
	}
}

func TestBoardTakeDelegatesPriorityAuthorizationToStore(t *testing.T) {
	f := &fakeBoardStore{}
	w := httptest.NewRecorder()
	body := `{"expected_version":7,"request_id":"` + boardTestRequestID + `","lease_id":"` + boardTestLeaseID + `","class":"human","why":"debugging board","duration_seconds":30}`
	boardTestMux(t, f, fakeNeutralVerifier{}).ServeHTTP(w, boardTestRequest("POST", "/v1/boards/ek-ra8d2/take", body))
	cmd, ok := f.command.(board.Enqueue)
	if w.Code != http.StatusOK || !ok || f.version != 7 || cmd.Waiter.ID != boardTestRequestID || cmd.Waiter.LeaseID != boardTestLeaseID || cmd.Waiter.Class != board.ClassHuman || cmd.Waiter.Duration != 30*time.Second || f.neutral != nil {
		t.Fatalf("take was not passed as a fenced enqueue: status=%d cmd=%#v version=%d", w.Code, f.command, f.version)
	}
	// The request cannot claim a principal; the store binds holder and checks
	// that the certificate's kind is allowed to request the selected class.
	if strings.Contains(body, "holder") {
		t.Fatal("test request unexpectedly supplied identity")
	}
}

func TestBoardRejectsClaimedIdentityAndTrailingJSON(t *testing.T) {
	f := &fakeBoardStore{}
	base := `{"expected_version":0,"request_id":"` + boardTestRequestID + `","lease_id":"` + boardTestLeaseID + `","class":"human","why":"test","duration_seconds":30`
	for _, body := range []string{base + `,"holder":"admin"}`, base + `} {}`, base + `,"neutral":true}`} {
		w := httptest.NewRecorder()
		boardTestMux(t, f, fakeNeutralVerifier{}).ServeHTTP(w, boardTestRequest("POST", "/v1/boards/ek-ra8d2/take", body))
		if w.Code != http.StatusBadRequest || f.applies != 0 {
			t.Fatalf("untrusted body reached store: status=%d applies=%d body=%s", w.Code, f.applies, body)
		}
	}
}

func TestBoardQueueDoesNotRequireNeutralReceiptVerifier(t *testing.T) {
	f := &fakeBoardStore{}
	body := `{"expected_version":0,"request_id":"` + boardTestRequestID + `","lease_id":"` + boardTestLeaseID + `","class":"human","why":"wait for the board","duration_seconds":30}`
	w := httptest.NewRecorder()
	boardTestMux(t, f, nil).ServeHTTP(w, boardTestRequest("POST", "/v1/boards/ek-ra8d2/take", body))
	command, ok := f.command.(board.Enqueue)
	if w.Code != http.StatusOK || !ok || f.applies != 1 || command.Waiter.Class != board.ClassHuman {
		t.Fatalf("safe queue request was blocked without a release verifier: status=%d applies=%d command=%#v", w.Code, f.applies, f.command)
	}
}

func TestBoardFreeFailsClosedWithoutNeutralVerifier(t *testing.T) {
	f := &fakeBoardStore{}
	w := httptest.NewRecorder()
	boardTestMux(t, f, nil).ServeHTTP(w, boardTestRequest("POST", "/v1/boards/ek-ra8d2/leases/"+boardTestLeaseID+"/free", `{}`))
	if w.Code != http.StatusServiceUnavailable || f.applies != 0 {
		t.Fatalf("release accepted without verifier: status=%d applies=%d", w.Code, f.applies)
	}
}

func TestBoardFreePassesOpaqueProofOnlyToStore(t *testing.T) {
	f := &fakeBoardStore{}
	verifier := fakeNeutralVerifier{}
	body := `{"expected_version":8,"generation":3,"challenge_id":"` + boardTestProofID + `","receipt":"c2lnbmVkLXJlY2VpcHQ="}`
	w := httptest.NewRecorder()
	boardTestMux(t, f, verifier).ServeHTTP(w, boardTestRequest("POST", "/v1/boards/ek-ra8d2/leases/"+boardTestLeaseID+"/free", body))
	cmd, ok := f.command.(board.Release)
	if w.Code != http.StatusOK || !ok || cmd.LeaseID != boardTestLeaseID || cmd.Generation != 3 || cmd.NeutralReceipt != "" || f.version != 8 || f.neutral == nil || f.neutral.ChallengeID != boardTestProofID || !bytes.Equal(f.neutral.Receipt, []byte("signed-receipt")) || f.verifier != verifier {
		t.Fatalf("neutral proof was not delegated to store verification: status=%d cmd=%#v neutral=%#v", w.Code, f.command, f.neutral)
	}
	for _, invalid := range []string{
		`{"expected_version":8,"generation":3,"challenge_id":"` + boardTestProofID + `","receipt":"c2lnbmVkLXJlY2VpcHQ=","neutral":true}`,
		`{"expected_version":8,"generation":3,"challenge_id":"` + boardTestProofID + `","receipt":""}`,
	} {
		w = httptest.NewRecorder()
		boardTestMux(t, f, verifier).ServeHTTP(w, boardTestRequest("POST", "/v1/boards/ek-ra8d2/leases/"+boardTestLeaseID+"/free", invalid))
		if w.Code != http.StatusBadRequest || f.applies != 1 {
			t.Fatalf("unproven release reached store: status=%d applies=%d", w.Code, f.applies)
		}
	}
}

func TestBoardChallengeRequiresConfiguredVerifierAndValidPurpose(t *testing.T) {
	f := &fakeBoardStore{}
	path := "/v1/boards/ek-ra8d2/neutral-challenge"
	w := httptest.NewRecorder()
	boardTestMux(t, f, nil).ServeHTTP(w, boardTestRequest("POST", path, `{"expected_version":1,"purpose":"release"}`))
	if w.Code != http.StatusServiceUnavailable || f.challenges != 0 {
		t.Fatalf("challenge issued without verifier: %d", w.Code)
	}
	w = httptest.NewRecorder()
	boardTestMux(t, f, fakeNeutralVerifier{}).ServeHTTP(w, boardTestRequest("POST", path, `{"expected_version":1,"purpose":"invented"}`))
	if w.Code != http.StatusBadRequest || f.challenges != 0 {
		t.Fatalf("invalid challenge purpose accepted: %d", w.Code)
	}
	w = httptest.NewRecorder()
	boardTestMux(t, f, fakeNeutralVerifier{}).ServeHTTP(w, boardTestRequest("POST", path, `{"expected_version":1,"purpose":"release"}`))
	if w.Code != http.StatusCreated || f.challenges != 1 || f.version != 1 {
		t.Fatalf("valid challenge was not delegated: status=%d calls=%d", w.Code, f.challenges)
	}
}

func TestBoardCommandFailureNeverReturnsSuccess(t *testing.T) {
	f := &fakeBoardStore{applyErr: &board.Error{Code: board.StaleGeneration, Detail: "stale"}}
	w := httptest.NewRecorder()
	boardTestMux(t, f, nil).ServeHTTP(w, boardTestRequest("POST", "/v1/boards/ek-ra8d2/checkpoint", `{"expected_version":2,"lease_id":"`+boardTestLeaseID+`","generation":1}`))
	if w.Code != http.StatusConflict || f.applies != 1 {
		t.Fatalf("stale command was reported successful: status=%d applies=%d", w.Code, f.applies)
	}
	f.applyErr = store.ErrDenied
	w = httptest.NewRecorder()
	boardTestMux(t, f, nil).ServeHTTP(w, boardTestRequest("POST", "/v1/boards/ek-ra8d2/checkpoint", `{"expected_version":2,"lease_id":"`+boardTestLeaseID+`","generation":1}`))
	if w.Code != http.StatusNotFound {
		t.Fatalf("denied command leaked existence: status=%d", w.Code)
	}
	f.applyErr = errors.New("database offline")
	w = httptest.NewRecorder()
	boardTestMux(t, f, nil).ServeHTTP(w, boardTestRequest("POST", "/v1/boards/ek-ra8d2/checkpoint", `{"expected_version":2,"lease_id":"`+boardTestLeaseID+`","generation":1}`))
	if w.Code != http.StatusServiceUnavailable {
		t.Fatalf("storage failure was reported successful: status=%d", w.Code)
	}
}

func TestUnboundRecoveryFinishIsNotExposed(t *testing.T) {
	f := &fakeBoardStore{}
	w := httptest.NewRecorder()
	boardTestMux(t, f, fakeNeutralVerifier{}).ServeHTTP(w,
		boardTestRequest("POST", "/v1/boards/ek-ra8d2/recovery/finish", `{"expected_version":1,"agent_high_water":999}`))
	if w.Code != http.StatusNotFound || f.applies != 0 {
		t.Fatalf("unbound recovery completion is exposed: status=%d applies=%d", w.Code, f.applies)
	}
}

func TestBoardCommandRoutesBuildTypedCommands(t *testing.T) {
	cases := []struct {
		name, path, body string
		check            func(board.Command) bool
	}{
		{"cancel", "/v1/boards/ek-ra8d2/waiters/" + boardTestRequestID + "/cancel", `{"expected_version":5}`,
			func(c board.Command) bool {
				v, ok := c.(board.CancelWaiter)
				return ok && v.WaiterID == boardTestRequestID
			}},
		{"checkpoint", "/v1/boards/ek-ra8d2/checkpoint", `{"expected_version":5,"lease_id":"` + boardTestLeaseID + `","generation":2}`,
			func(c board.Command) bool {
				v, ok := c.(board.BeginDrain)
				return ok && v.LeaseID == boardTestLeaseID && v.Generation == 2
			}},
		{"extend", "/v1/boards/ek-ra8d2/leases/" + boardTestLeaseID + "/extend", `{"expected_version":5,"generation":2,"new_expiry":"2026-09-23T00:00:00Z","why":"wrap up"}`,
			func(c board.Command) bool {
				v, ok := c.(board.Extend)
				return ok && v.LeaseID == boardTestLeaseID && v.Generation == 2 && v.Reason == "wrap up" && !v.NewExpiry.IsZero()
			}},
		{"agent_ack", "/v1/boards/ek-ra8d2/agent/ack", `{"expected_version":5,"lease_id":"` + boardTestLeaseID + `","generation":2,"installed_generation":2}`,
			func(c board.Command) bool {
				v, ok := c.(board.AcknowledgeGrant)
				return ok && v.Generation == 2 && v.InstalledGeneration == 2
			}},
		{"agent_observe", "/v1/boards/ek-ra8d2/agent/observe", `{"expected_version":5,"high_water":2}`,
			func(c board.Command) bool { v, ok := c.(board.ObserveAgentGeneration); return ok && v.HighWater == 2 }},
		{"agent_unavailable", "/v1/boards/ek-ra8d2/agent/unavailable", `{"expected_version":5,"why":"lost heartbeat"}`,
			func(c board.Command) bool {
				v, ok := c.(board.AgentUnavailable)
				return ok && v.Reason == "lost heartbeat"
			}},
		{"recovery_start", "/v1/boards/ek-ra8d2/recovery/start", `{"expected_version":5,"plan_id":"` + boardTestProofID + `","why":"restore fixture"}`,
			func(c board.Command) bool {
				v, ok := c.(board.BeginRecovery)
				return ok && v.PlanID == boardTestProofID && v.Reason == "restore fixture"
			}},
		{"quarantine", "/v1/boards/ek-ra8d2/quarantine", `{"expected_version":5,"why":"untrusted state"}`,
			func(c board.Command) bool { v, ok := c.(board.Quarantine); return ok && v.Reason == "untrusted state" }},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			f := &fakeBoardStore{}
			w := httptest.NewRecorder()
			boardTestMux(t, f, fakeNeutralVerifier{}).ServeHTTP(w, boardTestRequest("POST", tc.path, tc.body))
			if w.Code != http.StatusOK || f.applies != 1 || f.version != 5 || !tc.check(f.command) || f.neutral != nil {
				t.Fatalf("route did not bind typed command: status=%d applies=%d version=%d cmd=%#v", w.Code, f.applies, f.version, f.command)
			}
		})
	}
}

func TestBoardRejectsMalformedCommandBodies(t *testing.T) {
	cases := []struct{ path, body string }{
		{"/v1/boards/ek-ra8d2/waiters/not-a-uuid/cancel", `{"expected_version":1}`},
		{"/v1/boards/ek-ra8d2/checkpoint", `{"expected_version":1,"lease_id":"` + boardTestLeaseID + `","generation":0}`},
		{"/v1/boards/ek-ra8d2/leases/" + boardTestLeaseID + "/extend", `{"expected_version":1,"generation":1,"new_expiry":"2026-09-23T00:00:00Z","why":""}`},
		{"/v1/boards/ek-ra8d2/agent/ack", `{"expected_version":1,"lease_id":"` + boardTestLeaseID + `","generation":1,"installed_generation":0}`},
		{"/v1/boards/ek-ra8d2/agent/unavailable", `{"expected_version":1,"why":""}`},
		{"/v1/boards/ek-ra8d2/recovery/start", `{"expected_version":1,"plan_id":"bad","why":"restore"}`},
		{"/v1/boards/ek-ra8d2/quarantine", `{"expected_version":1,"why":""}`},
	}
	for _, tc := range cases {
		f := &fakeBoardStore{}
		w := httptest.NewRecorder()
		boardTestMux(t, f, fakeNeutralVerifier{}).ServeHTTP(w, boardTestRequest("POST", tc.path, tc.body))
		if w.Code != http.StatusBadRequest || f.applies != 0 {
			t.Errorf("malformed command reached store: path=%s status=%d applies=%d", tc.path, w.Code, f.applies)
		}
	}
}
