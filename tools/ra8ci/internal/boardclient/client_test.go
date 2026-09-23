package boardclient

import (
	"context"
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/json"
	"encoding/pem"
	"errors"
	"math/big"
	"net"
	"net/http"
	"net/http/httptest"
	"net/url"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

const (
	testRequestID = "01996f90-3415-7cfe-8ff1-600058131afd"
	testLeaseID   = "01996f90-3415-7cfe-8ff1-600058131afe"
	testProofID   = "01996f90-3415-7cfe-8ff1-600058131aff"
)

func testClient(t *testing.T, handler http.HandlerFunc) (*Client, func()) {
	t.Helper()
	server := httptest.NewServer(handler)
	base, err := url.Parse(server.URL)
	if err != nil {
		server.Close()
		t.Fatal(err)
	}
	return &Client{base: base, http: server.Client(), poll: time.Millisecond}, server.Close
}

func jsonResponse(w http.ResponseWriter, status int, value any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(value)
}

func transition(t *testing.T, state board.Snapshot, command board.Command) board.Snapshot {
	t.Helper()
	result, _, err := board.Apply(state, command, time.Now().UTC())
	if err != nil {
		t.Fatal(err)
	}
	return result
}

func activeBoard(t *testing.T) board.Snapshot {
	t.Helper()
	state, err := board.New("ek-ra8d2")
	if err != nil {
		t.Fatal(err)
	}
	state = transition(t, state, board.Enqueue{Actor: "human", Waiter: board.Waiter{
		ID: testRequestID, LeaseID: testLeaseID, Holder: "human", Class: board.ClassHuman,
		Reason: "test", Duration: time.Minute,
	}})
	state = transition(t, state, board.AcknowledgeGrant{Actor: "board-agent", LeaseID: testLeaseID,
		Generation: state.Generation, InstalledGeneration: state.Generation})
	return state
}

func testToken(state board.Snapshot) LeaseToken {
	return LeaseToken{BoardID: state.BoardID, RequestID: state.Lease.WaiterID,
		LeaseID: state.Lease.ID, Generation: state.Lease.Generation,
		ExpiresAt: state.Lease.ExpiresAt, Version: state.Version}
}

func TestNewFailsClosedOnMissingTLSMaterial(t *testing.T) {
	for _, config := range []Config{{}, {ServerURL: "http://localhost:8080", CAFile: "ca", CertFile: "cert", KeyFile: "key"},
		{ServerURL: "https://user:pass@localhost", CAFile: "ca", CertFile: "cert", KeyFile: "key"},
		{ServerURL: "https://localhost/prefix", CAFile: "ca", CertFile: "cert", KeyFile: "key"},
		{ServerURL: "https://localhost", CAFile: "/missing/ca", CertFile: "cert", KeyFile: "key"}} {
		if _, err := New(config); !errors.Is(err, ErrInvalidConfig) {
			t.Fatalf("configuration accepted without verified TLS: %+v err=%v", config, err)
		}
	}
}

func TestNewUsesMutualTLSAndRefusesRedirect(t *testing.T) {
	certs := testTLSCertificates(t)
	server := httptest.NewUnstartedServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.TLS == nil || len(r.TLS.VerifiedChains) == 0 || len(r.TLS.PeerCertificates) == 0 {
			t.Error("server did not authenticate client certificate")
			w.WriteHeader(http.StatusUnauthorized)
			return
		}
		if r.URL.Path == "/v1/boards/ek-ra8d2" {
			jsonResponse(w, http.StatusOK, board.Snapshot{BoardID: "ek-ra8d2", Phase: board.Ready})
			return
		}
		w.Header().Set("Location", "https://attacker.invalid/")
		w.WriteHeader(http.StatusFound)
	}))
	server.TLS = &tls.Config{Certificates: []tls.Certificate{certs.server}, ClientAuth: tls.RequireAndVerifyClientCert,
		ClientCAs: certs.pool, MinVersion: tls.VersionTLS13}
	server.StartTLS()
	defer server.Close()
	c, err := New(Config{ServerURL: server.URL, CAFile: certs.caPath,
		CertFile: certs.clientCertPath, KeyFile: certs.clientKeyPath})
	if err != nil {
		t.Fatal(err)
	}
	defer c.CloseIdleConnections()
	state, err := c.Status(context.Background(), "ek-ra8d2")
	if err != nil || state.BoardID != "ek-ra8d2" {
		t.Fatalf("mutual TLS status failed: state=%+v err=%v", state, err)
	}
	if err := c.request(context.Background(), http.MethodGet, "/redirect", nil, nil); err == nil {
		t.Fatal("redirect was followed or accepted")
	} else {
		var response *HTTPError
		if !errors.As(err, &response) || response.Status != http.StatusFound {
			t.Fatalf("redirect did not fail closed: %v", err)
		}
	}
}

type tlsCertificates struct {
	pool           *x509.CertPool
	server         tls.Certificate
	caPath         string
	clientCertPath string
	clientKeyPath  string
}

func testTLSCertificates(t *testing.T) tlsCertificates {
	t.Helper()
	dir := t.TempDir()
	caKey, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	now := time.Now()
	caTemplate := &x509.Certificate{SerialNumber: big.NewInt(1), Subject: pkix.Name{CommonName: "test CA"},
		NotBefore: now.Add(-time.Hour), NotAfter: now.Add(time.Hour), IsCA: true,
		BasicConstraintsValid: true, KeyUsage: x509.KeyUsageCertSign | x509.KeyUsageCRLSign}
	caDER, err := x509.CreateCertificate(rand.Reader, caTemplate, caTemplate, &caKey.PublicKey, caKey)
	if err != nil {
		t.Fatal(err)
	}
	ca, err := x509.ParseCertificate(caDER)
	if err != nil {
		t.Fatal(err)
	}
	caPEM := pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: caDER})
	caPath := filepath.Join(dir, "ca.pem")
	if err := os.WriteFile(caPath, caPEM, 0600); err != nil {
		t.Fatal(err)
	}
	makeCert := func(serial int64, server bool) (tls.Certificate, string, string) {
		t.Helper()
		key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
		if err != nil {
			t.Fatal(err)
		}
		template := &x509.Certificate{SerialNumber: big.NewInt(serial), NotBefore: now.Add(-time.Hour),
			NotAfter: now.Add(time.Hour), KeyUsage: x509.KeyUsageDigitalSignature}
		if server {
			template.Subject = pkix.Name{CommonName: "localhost"}
			template.DNSNames = []string{"localhost"}
			template.IPAddresses = []net.IP{net.ParseIP("127.0.0.1")}
			template.ExtKeyUsage = []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth}
		} else {
			template.Subject = pkix.Name{CommonName: "client"}
			template.ExtKeyUsage = []x509.ExtKeyUsage{x509.ExtKeyUsageClientAuth}
		}
		der, err := x509.CreateCertificate(rand.Reader, template, ca, &key.PublicKey, caKey)
		if err != nil {
			t.Fatal(err)
		}
		privateKey, err := x509.MarshalECPrivateKey(key)
		if err != nil {
			t.Fatal(err)
		}
		certPEM := pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: der})
		keyPEM := pem.EncodeToMemory(&pem.Block{Type: "EC PRIVATE KEY", Bytes: privateKey})
		certPath := filepath.Join(dir, "cert-"+string(rune('0'+serial))+".pem")
		keyPath := filepath.Join(dir, "key-"+string(rune('0'+serial))+".pem")
		if err := os.WriteFile(certPath, certPEM, 0600); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(keyPath, keyPEM, 0600); err != nil {
			t.Fatal(err)
		}
		pair, err := tls.LoadX509KeyPair(certPath, keyPath)
		if err != nil {
			t.Fatal(err)
		}
		return pair, certPath, keyPath
	}
	serverPair, _, _ := makeCert(2, true)
	_, clientCert, clientKey := makeCert(3, false)
	pool := x509.NewCertPool()
	pool.AddCert(ca)
	return tlsCertificates{pool: pool, server: serverPair, caPath: caPath,
		clientCertPath: clientCert, clientKeyPath: clientKey}
}

func TestStatusRejectsMalformedAndUnauthorizedResponses(t *testing.T) {
	c, closeServer := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/v1/boards/ek-ra8d2" {
			t.Errorf("unexpected path %q", r.URL.Path)
		}
		jsonResponse(w, http.StatusNotFound, map[string]any{"code": "denied", "detail": "hidden"})
	})
	defer closeServer()
	if _, err := c.Status(context.Background(), "bad/board"); !errors.Is(err, ErrInvalidRequest) {
		t.Fatalf("invalid board ID accepted: %v", err)
	}
	if _, err := c.Status(context.Background(), "ek-ra8d2"); !errors.Is(err, ErrNotFound) {
		t.Fatalf("server denial was accepted: %v", err)
	}
	malformed, closeMalformed := testClient(t, func(w http.ResponseWriter, _ *http.Request) {
		jsonResponse(w, http.StatusOK, board.Snapshot{BoardID: "other", Phase: board.Ready})
	})
	defer closeMalformed()
	if _, err := malformed.Status(context.Background(), "ek-ra8d2"); !errors.Is(err, ErrInvalidRequest) {
		t.Fatalf("wrong-board snapshot was accepted: %v", err)
	}
}

func TestRequestTakeRetriesCASAndWaitsForAgentAcknowledgement(t *testing.T) {
	var mu sync.Mutex
	state, _ := board.New("ek-ra8d2")
	posts, polls := 0, 0
	c, closeServer := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		mu.Lock()
		defer mu.Unlock()
		switch {
		case r.Method == http.MethodGet && r.URL.Path == "/v1/boards/ek-ra8d2":
			if posts >= 2 {
				polls++
				if polls == 2 {
					state = transition(t, state, board.AcknowledgeGrant{Actor: "board-agent", LeaseID: state.Lease.ID,
						Generation: state.Generation, InstalledGeneration: state.Generation})
				}
			}
			jsonResponse(w, http.StatusOK, state)
		case r.Method == http.MethodPost && r.URL.Path == "/v1/boards/ek-ra8d2/take":
			posts++
			if posts == 1 {
				jsonResponse(w, http.StatusConflict, map[string]any{"code": "conflict"})
				return
			}
			var request struct {
				ExpectedVersion uint64 `json:"expected_version"`
				RequestID       string `json:"request_id"`
				LeaseID         string `json:"lease_id"`
				Class           string `json:"class"`
				Why             string `json:"why"`
				DurationSeconds int64  `json:"duration_seconds"`
			}
			if err := json.NewDecoder(r.Body).Decode(&request); err != nil || request.ExpectedVersion != state.Version ||
				request.Class != "human" || request.DurationSeconds != 30 || !store.ValidID(request.RequestID) || !store.ValidID(request.LeaseID) {
				t.Errorf("malformed take request: %+v err=%v", request, err)
			}
			state = transition(t, state, board.Enqueue{Actor: "human", Waiter: board.Waiter{
				ID: request.RequestID, LeaseID: request.LeaseID, Holder: "human",
				Class: board.ClassHuman, Reason: request.Why, Duration: time.Duration(request.DurationSeconds) * time.Second,
			}})
			jsonResponse(w, http.StatusOK, commandResponse{Snapshot: state})
		default:
			t.Errorf("unexpected route %s %s", r.Method, r.URL.Path)
			w.WriteHeader(http.StatusNotFound)
		}
	})
	defer closeServer()
	ctx, cancel := context.WithTimeout(context.Background(), time.Second)
	defer cancel()
	ticket, err := c.RequestTake(ctx, "ek-ra8d2", board.ClassHuman, "debug", 30*time.Second)
	if err != nil || !store.ValidID(ticket.RequestID) || !store.ValidID(ticket.LeaseID) {
		t.Fatalf("take failed: ticket=%+v err=%v", ticket, err)
	}
	token, err := c.WaitForGrant(ctx, ticket)
	if err != nil || token.Generation != 1 || token.LeaseID != ticket.LeaseID {
		t.Fatalf("wait returned without matching active grant: token=%+v err=%v", token, err)
	}
	mu.Lock()
	defer mu.Unlock()
	if posts != 2 || polls < 2 || state.Phase != board.Active {
		t.Fatalf("CAS/agent acknowledgement not observed: posts=%d polls=%d phase=%s", posts, polls, state.Phase)
	}
}

type receiptProducer struct {
	seen    store.NeutralChallenge
	receipt []byte
	err     error
}

func (p *receiptProducer) ProduceNeutralReceipt(_ context.Context, challenge store.NeutralChallenge) ([]byte, error) {
	p.seen = challenge
	return p.receipt, p.err
}

func TestFreeRequiresOneUseSignedChallenge(t *testing.T) {
	var mu sync.Mutex
	state := activeBoard(t)
	token := testToken(state)
	challengeCalls, freeCalls := 0, 0
	producer := &receiptProducer{receipt: []byte("board-agent-signature")}
	c, closeServer := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		mu.Lock()
		defer mu.Unlock()
		switch r.URL.Path {
		case "/v1/boards/ek-ra8d2":
			jsonResponse(w, http.StatusOK, state)
		case "/v1/boards/ek-ra8d2/neutral-challenge":
			challengeCalls++
			jsonResponse(w, http.StatusCreated, store.NeutralChallenge{
				ID: testProofID, Nonce: "nonce", BoardID: state.BoardID, Purpose: "release",
				LeaseID: state.Lease.ID, Generation: state.Generation, SnapshotVersion: state.Version,
				ProfileSHA256: "profile", FixtureRevision: "fixture", ExpiresAt: time.Now().Add(time.Minute),
			})
		case "/v1/boards/ek-ra8d2/leases/" + testLeaseID + "/free":
			freeCalls++
			var submission struct {
				ExpectedVersion uint64 `json:"expected_version"`
				ChallengeID     string `json:"challenge_id"`
				Receipt         []byte `json:"receipt"`
				Neutral         *bool  `json:"neutral"`
			}
			if err := json.NewDecoder(r.Body).Decode(&submission); err != nil || submission.Neutral != nil ||
				submission.ChallengeID != testProofID || string(submission.Receipt) != "board-agent-signature" ||
				submission.ExpectedVersion != state.Version {
				t.Errorf("untrusted release payload: %+v err=%v", submission, err)
			}
			state = transition(t, state, board.Release{Actor: "human", LeaseID: state.Lease.ID,
				Generation: state.Generation, NeutralReceipt: "verified-by-server"})
			jsonResponse(w, http.StatusOK, commandResponse{Snapshot: state})
		default:
			t.Errorf("unexpected route %s", r.URL.Path)
			w.WriteHeader(http.StatusNotFound)
		}
	})
	defer closeServer()
	if _, err := c.Free(context.Background(), token, nil); !errors.Is(err, ErrNeutralUnavailable) {
		t.Fatalf("release without producer was accepted: %v", err)
	}
	result, err := c.Free(context.Background(), token, producer)
	if err != nil || result.Phase != board.Ready || producer.seen.ID != testProofID {
		t.Fatalf("neutral release failed: phase=%s challenge=%+v err=%v", result.Phase, producer.seen, err)
	}
	mu.Lock()
	defer mu.Unlock()
	if challengeCalls != 1 || freeCalls != 1 {
		t.Fatalf("unexpected challenge/release count: %d/%d", challengeCalls, freeCalls)
	}
}

func TestFreeRejectsUnboundChallengeOrEmptyReceipt(t *testing.T) {
	state := activeBoard(t)
	token := testToken(state)
	freeCalls := 0
	c, closeServer := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		switch {
		case r.Method == http.MethodGet:
			jsonResponse(w, http.StatusOK, state)
		case strings.HasSuffix(r.URL.Path, "/neutral-challenge"):
			jsonResponse(w, http.StatusCreated, store.NeutralChallenge{ID: testProofID, Nonce: "nonce",
				BoardID: "other", Purpose: "release", LeaseID: testLeaseID, Generation: 1,
				SnapshotVersion: state.Version, ProfileSHA256: "profile", FixtureRevision: "fixture",
				ExpiresAt: time.Now().Add(time.Minute)})
		default:
			freeCalls++
			w.WriteHeader(http.StatusOK)
		}
	})
	defer closeServer()
	if _, err := c.Free(context.Background(), token, &receiptProducer{receipt: []byte("signed")}); !errors.Is(err, ErrInvalidNeutralProof) {
		t.Fatalf("unbound challenge accepted: %v", err)
	}
	if freeCalls != 0 {
		t.Fatal("release request sent for unbound challenge")
	}
}

func TestCancelOnlyWithdrawsQueuedWaiter(t *testing.T) {
	state := activeBoard(t)
	state = transition(t, state, board.Enqueue{Actor: "agent", Waiter: board.Waiter{
		ID: testProofID, LeaseID: "01996f90-3415-7cfe-8ff1-600058131af0", Holder: "agent",
		Class: board.ClassAI, Reason: "waiting", Duration: time.Minute,
	}})
	ticket := Ticket{BoardID: state.BoardID, RequestID: testProofID, LeaseID: "01996f90-3415-7cfe-8ff1-600058131af0"}
	posts := 0
	c, closeServer := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		switch r.Method {
		case http.MethodGet:
			jsonResponse(w, http.StatusOK, state)
		case http.MethodPost:
			posts++
			if posts == 1 {
				jsonResponse(w, http.StatusConflict, map[string]any{"code": "conflict"})
				return
			}
			if !strings.HasSuffix(r.URL.Path, "/waiters/"+testProofID+"/cancel") {
				t.Errorf("unexpected cancel path %s", r.URL.Path)
			}
			state = transition(t, state, board.CancelWaiter{Actor: "agent", WaiterID: testProofID})
			jsonResponse(w, http.StatusOK, commandResponse{Snapshot: state})
		}
	})
	defer closeServer()
	ctx, cancel := context.WithTimeout(context.Background(), time.Second)
	defer cancel()
	if err := c.Cancel(ctx, ticket); err != nil || posts != 2 || len(state.Queue) != 0 {
		t.Fatalf("CAS cancel failed: err=%v posts=%d queue=%d", err, posts, len(state.Queue))
	}
	if err := c.Cancel(ctx, ticket); err != nil {
		t.Fatalf("repeat cancellation was not idempotent: %v", err)
	}
	if err := c.Cancel(ctx, Ticket{BoardID: state.BoardID, RequestID: testRequestID, LeaseID: testLeaseID}); !errors.Is(err, ErrAlreadyGranted) {
		t.Fatalf("granted lease was cancelled: %v", err)
	}
}

func TestCheckpointAndExtendUseCurrentVersion(t *testing.T) {
	state := activeBoard(t)
	token := testToken(state)
	postCount := 0
	c, closeServer := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		if r.Method == http.MethodGet {
			jsonResponse(w, http.StatusOK, state)
			return
		}
		postCount++
		if strings.HasSuffix(r.URL.Path, "/extend") {
			var req struct {
				ExpectedVersion uint64    `json:"expected_version"`
				NewExpiry       time.Time `json:"new_expiry"`
			}
			if err := json.NewDecoder(r.Body).Decode(&req); err != nil || req.ExpectedVersion != state.Version {
				t.Errorf("stale extension: %+v err=%v", req, err)
			}
			state = transition(t, state, board.Extend{Actor: "human", LeaseID: testLeaseID,
				Generation: state.Generation, NewExpiry: req.NewExpiry, Reason: "more time"})
			jsonResponse(w, http.StatusOK, commandResponse{Snapshot: state})
			return
		}
		if r.URL.Path == "/v1/boards/ek-ra8d2/checkpoint" {
			var req struct {
				ExpectedVersion uint64 `json:"expected_version"`
			}
			if err := json.NewDecoder(r.Body).Decode(&req); err != nil || req.ExpectedVersion != state.Version {
				t.Errorf("stale checkpoint: %+v err=%v", req, err)
			}
			state = transition(t, state, board.BeginDrain{Actor: "human", LeaseID: testLeaseID,
				Generation: state.Generation})
			jsonResponse(w, http.StatusOK, commandResponse{Snapshot: state})
			return
		}
		t.Errorf("unexpected route %s", r.URL.Path)
		w.WriteHeader(http.StatusNotFound)
	})
	defer closeServer()
	ctx, cancel := context.WithTimeout(context.Background(), time.Second)
	defer cancel()
	if err := c.CanStartSegment(ctx, token, time.Second, time.Second); err != nil {
		t.Fatalf("valid active segment denied: %v", err)
	}
	if _, err := c.Checkpoint(ctx, token); !errors.Is(err, ErrNoYieldRequest) {
		t.Fatalf("checkpoint without yield unexpectedly proceeded: %v", err)
	}
	newExpiry := state.Lease.ExpiresAt.Add(30 * time.Second)
	result, err := c.Extend(ctx, token, newExpiry, "more time")
	if err != nil || !result.Lease.ExpiresAt.Equal(newExpiry) || postCount != 1 {
		t.Fatalf("extension failed: err=%v posts=%d", err, postCount)
	}
	// A higher-priority waiter forces a cooperative checkpoint on an AI lease.
	// Build that state independently because a human lease cannot be outranked.
}

func TestCheckpointAfterHigherPriorityWaiter(t *testing.T) {
	state, _ := board.New("ek-ra8d2")
	state = transition(t, state, board.Enqueue{Actor: "agent", Waiter: board.Waiter{
		ID: testRequestID, LeaseID: testLeaseID, Holder: "agent", Class: board.ClassAI,
		Reason: "automation", Duration: time.Minute,
	}})
	state = transition(t, state, board.AcknowledgeGrant{Actor: "board-agent", LeaseID: testLeaseID,
		Generation: state.Generation, InstalledGeneration: state.Generation})
	token := testToken(state)
	state = transition(t, state, board.Enqueue{Actor: "human", Waiter: board.Waiter{
		ID: testProofID, LeaseID: "01996f90-3415-7cfe-8ff1-600058131af0", Holder: "human",
		Class: board.ClassHuman, Reason: "urgent", Duration: time.Minute,
	}})
	if state.Phase != board.YieldRequested {
		t.Fatalf("higher-priority waiter did not request yield: %s", state.Phase)
	}
	c, closeServer := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		if r.Method == http.MethodGet {
			jsonResponse(w, http.StatusOK, state)
			return
		}
		state = transition(t, state, board.BeginDrain{Actor: "agent", LeaseID: testLeaseID,
			Generation: state.Generation})
		jsonResponse(w, http.StatusOK, commandResponse{Snapshot: state})
	})
	defer closeServer()
	if err := c.CanStartSegment(context.Background(), token, time.Second, time.Second); err == nil {
		t.Fatal("new work started despite queued human")
	}
	result, err := c.Checkpoint(context.Background(), token)
	if err != nil || result.Phase != board.Draining {
		t.Fatalf("cooperative checkpoint failed: phase=%s err=%v", result.Phase, err)
	}
	result, err = c.Checkpoint(context.Background(), token)
	if err != nil || result.Phase != board.Draining {
		t.Fatalf("repeat checkpoint was not idempotent: phase=%s err=%v", result.Phase, err)
	}
}

func TestWaitForGrantNeverTreatsPendingYieldOrRecoveryAsAuthority(t *testing.T) {
	pending, _ := board.New("ek-ra8d2")
	pending = transition(t, pending, board.Enqueue{Actor: "agent", Waiter: board.Waiter{
		ID: testRequestID, LeaseID: testLeaseID, Holder: "agent", Class: board.ClassAI,
		Reason: "work", Duration: time.Minute,
	}})
	active := transition(t, pending, board.AcknowledgeGrant{Actor: "board-agent", LeaseID: testLeaseID,
		Generation: pending.Generation, InstalledGeneration: pending.Generation})
	yielded := transition(t, active, board.Enqueue{Actor: "human", Waiter: board.Waiter{
		ID: testProofID, LeaseID: "01996f90-3415-7cfe-8ff1-600058131af0", Holder: "human",
		Class: board.ClassHuman, Reason: "priority", Duration: time.Minute,
	}})
	recovery := transition(t, active, board.AgentUnavailable{Actor: "board-agent", Reason: "offline"})
	missing, _ := board.New("ek-ra8d2")
	expired := active
	copyLease := *active.Lease
	copyLease.GrantedAt = time.Now().Add(-2 * time.Minute)
	copyLease.ExpiresAt = time.Now().Add(-time.Second)
	expired.Lease = &copyLease
	ticket := Ticket{BoardID: "ek-ra8d2", RequestID: testRequestID, LeaseID: testLeaseID}
	for _, tc := range []struct {
		name   string
		state  board.Snapshot
		want   error
		grants bool
	}{
		{"yield", yielded, ErrYieldRequested, true},
		{"recovery", recovery, ErrRecoveryRequired, false},
		{"missing", missing, ErrNotQueued, false},
		{"expired", expired, ErrRecoveryRequired, false},
	} {
		t.Run(tc.name, func(t *testing.T) {
			c, closeServer := testClient(t, func(w http.ResponseWriter, _ *http.Request) {
				jsonResponse(w, http.StatusOK, tc.state)
			})
			defer closeServer()
			token, err := c.WaitForGrant(context.Background(), ticket)
			if !errors.Is(err, tc.want) || (token.Generation != 0) != tc.grants {
				t.Fatalf("unsafe state accepted: token=%+v err=%v", token, err)
			}
		})
	}
	c, closeServer := testClient(t, func(w http.ResponseWriter, _ *http.Request) {
		jsonResponse(w, http.StatusOK, pending)
	})
	defer closeServer()
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Millisecond)
	defer cancel()
	if _, err := c.WaitForGrant(ctx, ticket); !errors.Is(err, context.DeadlineExceeded) {
		t.Fatalf("pending grant escaped context deadline: %v", err)
	}
}

func TestRequestTakeValidatesClassDurationAndPreservesTicketOnFailure(t *testing.T) {
	for _, tc := range []struct {
		class    board.Class
		duration time.Duration
	}{
		{0, time.Second}, {board.ClassHuman, 9 * time.Hour},
		{board.ClassCI, 3 * time.Hour}, {board.ClassAI, 2 * time.Hour},
		{board.ClassAI, time.Millisecond},
	} {
		if ticket, err := (&Client{}).RequestTake(context.Background(), "ek-ra8d2", tc.class, "test", tc.duration); !errors.Is(err, ErrInvalidRequest) || ticket.RequestID != "" {
			t.Errorf("invalid class/duration accepted: class=%d duration=%s ticket=%+v err=%v", tc.class, tc.duration, ticket, err)
		}
	}
	for _, class := range []board.Class{board.ClassCI, board.ClassAI} {
		c, closeServer := testClient(t, func(w http.ResponseWriter, r *http.Request) {
			if r.Method == http.MethodGet {
				jsonResponse(w, http.StatusOK, board.Snapshot{BoardID: "ek-ra8d2", Phase: board.Ready})
				return
			}
			var request struct {
				Class string `json:"class"`
			}
			_ = json.NewDecoder(r.Body).Decode(&request)
			if request.Class != takeClassName(class) {
				t.Errorf("wrong requested class %q", request.Class)
			}
			jsonResponse(w, http.StatusServiceUnavailable, map[string]any{"code": "unavailable"})
		})
		ticket, err := c.RequestTake(context.Background(), "ek-ra8d2", class, "test", time.Second)
		closeServer()
		if ticket.Class != class || !store.ValidID(ticket.RequestID) || err == nil {
			t.Errorf("ambiguous ticket not preserved: ticket=%+v err=%v", ticket, err)
		}
	}
}

func TestFreeRejectsProducerFailureAndRetriesStaleChallenge(t *testing.T) {
	state := activeBoard(t)
	token := testToken(state)
	challengeCalls, freeCalls := 0, 0
	c, closeServer := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		switch {
		case r.Method == http.MethodGet:
			jsonResponse(w, http.StatusOK, state)
		case strings.HasSuffix(r.URL.Path, "/neutral-challenge"):
			challengeCalls++
			if challengeCalls == 1 {
				jsonResponse(w, http.StatusConflict, map[string]any{"code": "conflict"})
				return
			}
			jsonResponse(w, http.StatusCreated, store.NeutralChallenge{
				ID: testProofID, Nonce: "nonce", BoardID: state.BoardID, Purpose: "release",
				LeaseID: testLeaseID, Generation: state.Generation, SnapshotVersion: state.Version,
				ProfileSHA256: "profile", FixtureRevision: "fixture", ExpiresAt: time.Now().Add(time.Minute),
			})
		default:
			freeCalls++
			w.WriteHeader(http.StatusOK)
		}
	})
	defer closeServer()
	producerErr := errors.New("board agent offline")
	if _, err := c.Free(context.Background(), token, &receiptProducer{err: producerErr}); !errors.Is(err, producerErr) {
		t.Fatalf("board-agent error was ignored: %v", err)
	}
	if _, err := c.Free(context.Background(), token, &receiptProducer{}); !errors.Is(err, ErrInvalidNeutralProof) {
		t.Fatalf("empty receipt was accepted: %v", err)
	}
	if challengeCalls != 3 || freeCalls != 0 {
		t.Fatalf("unproved release sent: challenges=%d releases=%d", challengeCalls, freeCalls)
	}
}

func TestHTTPErrorAndResponseLimits(t *testing.T) {
	err := &HTTPError{Status: http.StatusConflict, Code: "conflict", Detail: "stale"}
	if !strings.Contains(err.Error(), "stale") || !errors.Is(err, &HTTPError{Status: http.StatusConflict}) || errors.Is(err, ErrNotFound) {
		t.Fatal("typed HTTP error semantics are wrong")
	}
	c, closeServer := testClient(t, func(w http.ResponseWriter, _ *http.Request) {
		w.WriteHeader(http.StatusOK)
		_, _ = w.Write([]byte(strings.Repeat("x", maxResponseBytes+1)))
	})
	defer closeServer()
	if _, err := c.Status(context.Background(), "ek-ra8d2"); !errors.Is(err, ErrInvalidRequest) {
		t.Fatalf("oversized response accepted: %v", err)
	}
}

func TestExtendRetriesVersionConflictButRejectsStaleAuthority(t *testing.T) {
	state := activeBoard(t)
	token := testToken(state)
	posts := 0
	c, closeServer := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		if r.Method == http.MethodGet {
			jsonResponse(w, http.StatusOK, state)
			return
		}
		posts++
		if posts == 1 {
			jsonResponse(w, http.StatusConflict, map[string]any{"code": "conflict"})
			return
		}
		var req struct {
			ExpectedVersion uint64    `json:"expected_version"`
			NewExpiry       time.Time `json:"new_expiry"`
		}
		if err := json.NewDecoder(r.Body).Decode(&req); err != nil || req.ExpectedVersion != state.Version {
			t.Errorf("stale retry: %+v err=%v", req, err)
		}
		state = transition(t, state, board.Extend{Actor: "human", LeaseID: testLeaseID,
			Generation: state.Generation, NewExpiry: req.NewExpiry, Reason: "wrap up"})
		jsonResponse(w, http.StatusOK, commandResponse{Snapshot: state})
	})
	defer closeServer()
	if _, err := c.Extend(context.Background(), token, time.Time{}, "wrap up"); !errors.Is(err, ErrInvalidRequest) {
		t.Fatalf("zero expiry accepted: %v", err)
	}
	if _, err := c.Extend(context.Background(), LeaseToken{}, time.Now().Add(time.Minute), "wrap up"); !errors.Is(err, ErrInvalidRequest) {
		t.Fatalf("empty lease token accepted: %v", err)
	}
	stale := token
	stale.Generation++
	if _, err := c.Extend(context.Background(), stale, time.Now().Add(time.Minute), "wrap up"); !errors.Is(err, ErrStaleLease) {
		t.Fatalf("stale generation accepted: %v", err)
	}
	result, err := c.Extend(context.Background(), token, state.Lease.ExpiresAt.Add(30*time.Second), "wrap up")
	if err != nil || posts != 2 || result.Lease.DeadlineVersion != 2 {
		t.Fatalf("CAS extension did not retry safely: err=%v posts=%d result=%+v", err, posts, result.Lease)
	}
}

func TestRequestTakeCanBootstrapAbsentBoard(t *testing.T) {
	state, _ := board.New("ek-ra8d2")
	created := false
	c, closeServer := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		if r.Method == http.MethodGet {
			if !created {
				jsonResponse(w, http.StatusNotFound, map[string]any{"code": "not_found"})
				return
			}
			jsonResponse(w, http.StatusOK, state)
			return
		}
		var req struct {
			ExpectedVersion uint64 `json:"expected_version"`
			RequestID       string `json:"request_id"`
			LeaseID         string `json:"lease_id"`
			Why             string `json:"why"`
		}
		if err := json.NewDecoder(r.Body).Decode(&req); err != nil || req.ExpectedVersion != 0 {
			t.Errorf("bootstrap did not use initial CAS: %+v err=%v", req, err)
		}
		state = transition(t, state, board.Enqueue{Actor: "human", Waiter: board.Waiter{
			ID: req.RequestID, LeaseID: req.LeaseID, Holder: "human", Class: board.ClassHuman,
			Reason: req.Why, Duration: time.Second,
		}})
		created = true
		jsonResponse(w, http.StatusOK, commandResponse{Snapshot: state})
	})
	defer closeServer()
	ticket, err := c.RequestTake(context.Background(), "ek-ra8d2", board.ClassHuman, "test", time.Second)
	if err != nil || !created || !visibleTicket(state, ticket) {
		t.Fatalf("absent board was not safely bootstrapped: ticket=%+v err=%v", ticket, err)
	}
}

func TestRequestTakeRejectsUnrelatedAcceptedSnapshot(t *testing.T) {
	state, _ := board.New("ek-ra8d2")
	c, closeServer := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		if r.Method == http.MethodGet {
			jsonResponse(w, http.StatusOK, state)
			return
		}
		jsonResponse(w, http.StatusOK, commandResponse{Snapshot: state})
	})
	defer closeServer()
	ticket, err := c.RequestTake(context.Background(), "ek-ra8d2", board.ClassHuman, "test", time.Second)
	if !errors.Is(err, ErrInvalidRequest) || !store.ValidID(ticket.RequestID) {
		t.Fatalf("unrelated board snapshot accepted: ticket=%+v err=%v", ticket, err)
	}
}

func TestCommandAndLeaseRejectMalformedServerState(t *testing.T) {
	c, closeServer := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		if r.Method == http.MethodGet {
			jsonResponse(w, http.StatusOK, activeBoard(t))
			return
		}
		jsonResponse(w, http.StatusOK, commandResponse{Snapshot: board.Snapshot{BoardID: "wrong", Phase: board.Ready}})
	})
	defer closeServer()
	state := activeBoard(t)
	token := testToken(state)
	if _, err := c.command(context.Background(), "ek-ra8d2", "/checkpoint", map[string]any{}); !errors.Is(err, ErrInvalidRequest) {
		t.Fatalf("wrong-board command result accepted: %v", err)
	}
	if _, err := c.leaseStatus(context.Background(), LeaseToken{}); !errors.Is(err, ErrInvalidRequest) {
		t.Fatalf("invalid token accepted: %v", err)
	}
	stale := token
	stale.RequestID = testProofID
	if _, err := c.leaseStatus(context.Background(), stale); !errors.Is(err, ErrStaleLease) {
		t.Fatalf("wrong waiter accepted: %v", err)
	}
}
