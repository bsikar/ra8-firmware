//go:build integration

package server

import (
	"bytes"
	"context"
	"crypto/sha256"
	"crypto/tls"
	"crypto/x509"
	"encoding/hex"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"net/url"
	"os"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/protocol"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/migrations"
	"github.com/jackc/pgx/v5/pgxpool"
)

// What the claim and heartbeat doors answer once the store is actually
// behind them.
//
// The pure half of these doors is already held by the_doors_an_agent_knocks_on_test.go
// (media types, undecodable bodies, unverified peers, the audit-before-answer
// ordering). What is left is the part only a database can show: an agent that
// polls an empty queue, an agent that is handed work, and the beat that keeps
// that work alive.

// agentWork is one registered agent and, when asked for, one queued run it is
// entitled to claim.
type agentWork struct {
	plane       http.Handler
	certificate *x509.Certificate
	repository  string
	commit      string
	facts       protocol.HostFacts
}

// claimingPlane builds a plane whose store is live, with one agent identity
// registered against a repository of its own. Each call mints a fresh
// certificate and a fresh repository, so tests never claim each other's work:
// the queue is global and a neighbour's queued task would otherwise be handed
// to the wrong agent and make a 204 test pass or fail for the wrong reason.
func claimingPlane(t *testing.T, withWork bool) agentWork {
	t.Helper()
	dsn := os.Getenv("RA8CI_TEST_PG_DSN")
	config, err := pgxpool.ParseConfig(dsn)
	if err != nil || config.ConnConfig.Host != "127.0.0.1" || config.ConnConfig.Database != "ra8ci_test" {
		t.Fatal("RA8CI_TEST_PG_DSN must identify disposable loopback ra8ci_test")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	pool, err := pgxpool.New(ctx, dsn)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(pool.Close)
	if err := migrations.Apply(ctx, pool); err != nil {
		t.Fatal(err)
	}
	grantRuntimeRole(t, pool)

	runtimeURL, err := url.Parse(dsn)
	if err != nil {
		t.Fatal(err)
	}
	runtimeURL.User = url.UserPassword("ra8ci_server_runtime_test", "ra8ci_server_runtime_test_only")
	st, err := store.Open(ctx, runtimeURL.String())
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(st.Close)
	cat, err := catalog.Load()
	if err != nil {
		t.Fatal(err)
	}

	certificate := testClientCertificate(t)
	fingerprint := sha256.Sum256(certificate.Raw)
	principal := "claim-agent-" + mustAgentID(t)
	repository := "bsikar/ra8ci-claim-test-" + mustAgentID(t)
	commit := strings.Repeat("a", 40)

	if _, err := pool.Exec(ctx, `INSERT INTO api_principals
		(cert_sha256,principal_id,kind,expires_at)
		VALUES ($1,$2,'agent',clock_timestamp()+interval '1 hour')`,
		hex.EncodeToString(fingerprint[:]), principal); err != nil {
		t.Fatal(err)
	}
	if _, err := pool.Exec(ctx, `INSERT INTO api_grants(principal_id,repository,role)
		VALUES ($1,$2,'agent_executor')`, principal, repository); err != nil {
		t.Fatal(err)
	}
	if _, err := pool.Exec(ctx, `INSERT INTO agents(id,principal_id,host_class,version,
		capabilities,capacity,state) VALUES ($1,$2,'linux-vm','test',
		'{"os":"linux"}'::jsonb,1,'healthy')`, mustAgentID(t), principal); err != nil {
		t.Fatal(err)
	}

	if withWork {
		if _, err := st.CreateRun(ctx, store.CreateRunInput{
			Trigger: "integration", ActorID: "integration-submitter",
			Repository: repository, Branch: "test",
			CommitSHA: commit, SnapshotSHA256: strings.Repeat("b", 64),
			CatalogSHA256: cat.Digest(),
			Tasks: []store.TaskInput{{Key: "format-check", Name: "format-check",
				Arguments: json.RawMessage(`{"argv":[]}`), Tier: "required",
				Scope: "safe-local-read-only", HostClass: "safe-local-read-only",
				DeadlineSeconds: 900}},
		}); err != nil {
			t.Fatal(err)
		}
	}

	// The trusted commit is what the claim is judged against, so it has to be
	// the commit the run actually carries.
	api, err := NewWithOptions(st, cat, nil, commit)
	if err != nil {
		t.Fatal(err)
	}
	return agentWork{
		plane:       api.Handler(),
		certificate: certificate,
		repository:  repository,
		commit:      commit,
		facts: protocol.HostFacts{Cores: 4, RAMBytes: 8 << 30, RAMFreeBytes: 4 << 30,
			Load1: 0.5, LoadKind: "linux_load1", OS: "linux", Arch: "amd64",
			CapturedAt: time.Now().UTC()},
	}
}

func mustAgentID(t *testing.T) string {
	t.Helper()
	id, err := store.NewID()
	if err != nil {
		t.Fatal(err)
	}
	return id
}

// agentKnock posts a document at one of the agent doors as the registered
// agent identity.
func (w agentWork) agentKnock(t *testing.T, path string, document any) *httptest.ResponseRecorder {
	t.Helper()
	body, err := json.Marshal(document)
	if err != nil {
		t.Fatal(err)
	}
	request := httptest.NewRequest(http.MethodPost, path, bytes.NewReader(body))
	request.Header.Set("Content-Type", "application/json")
	request.TLS = &tls.ConnectionState{
		PeerCertificates: []*x509.Certificate{w.certificate},
		VerifiedChains:   [][]*x509.Certificate{{w.certificate}},
	}
	recorder := httptest.NewRecorder()
	w.plane.ServeHTTP(recorder, request)
	return recorder
}

// claimOne takes the single queued task and hands back the fenced grant.
func (w agentWork) claimOne(t *testing.T) protocol.Assignment {
	t.Helper()
	result := w.agentKnock(t, "/v1/agents/me/claim", protocol.ClaimRequest{
		SchemaVersion: protocol.Version, HostFacts: w.facts, PollWaitMS: 0,
	})
	if result.Code != http.StatusOK {
		t.Fatalf("claim answered %d: %s", result.Code, result.Body.String())
	}
	var assignment protocol.Assignment
	if err := json.Unmarshal(result.Body.Bytes(), &assignment); err != nil {
		t.Fatalf("decode assignment: %v", err)
	}
	return assignment
}

// acknowledge is the step between claiming work and being allowed to report on
// it. An attempt that has not confirmed the grant and its verified local source
// is not yet running, so the beat door refuses it; every test below that beats
// has to come through here first, or it would pass on the wrong refusal.
func (w agentWork) acknowledge(t *testing.T, assignment protocol.Assignment) {
	t.Helper()
	result := w.agentKnock(t, "/v1/assignments/"+assignment.AssignmentID+"/ack", protocol.Ack{
		SchemaVersion:        protocol.Version,
		AssignmentID:         assignment.AssignmentID,
		AttemptID:            assignment.AttemptID,
		AssignmentVersion:    assignment.AssignmentVersion,
		FencingToken:         assignment.FencingToken,
		CatalogSHA256:        assignment.CatalogSHA256,
		SourceSnapshotSHA256: assignment.Source.SnapshotSHA256,
		HostFacts:            w.facts,
	})
	if result.Code != http.StatusOK {
		t.Fatalf("ack answered %d: %s", result.Code, result.Body.String())
	}
}

func TestIntegrationAnAgentPollingAnEmptyQueueIsToldSoAndNothingMore(t *testing.T) {
	work := claimingPlane(t, false)

	result := work.agentKnock(t, "/v1/agents/me/claim", protocol.ClaimRequest{
		SchemaVersion: protocol.Version, HostFacts: work.facts, PollWaitMS: 0,
	})

	// 204, not 200 with an empty object and not an error. An agent polls this
	// door forever, so "there is nothing for you" has to be cheap and
	// unambiguous: a 200 would make every idle poll decode a body, and a 5xx
	// would make an idle queue look like an outage.
	if result.Code != http.StatusNoContent {
		t.Fatalf("idle claim answered %d: %s", result.Code, result.Body.String())
	}
	if body := result.Body.String(); body != "" {
		t.Fatalf("a no-content answer carried a body: %q", body)
	}
	if contentType := result.Header().Get("Content-Type"); contentType != "" {
		t.Fatalf("a no-content answer named a media type: %q", contentType)
	}
}

func TestIntegrationAClaimedAssignmentIsFencedAndDeadlined(t *testing.T) {
	work := claimingPlane(t, true)

	result := work.agentKnock(t, "/v1/agents/me/claim", protocol.ClaimRequest{
		SchemaVersion: protocol.Version, HostFacts: work.facts, PollWaitMS: 0,
	})
	if result.Code != http.StatusOK {
		t.Fatalf("claim answered %d: %s", result.Code, result.Body.String())
	}
	var assignment protocol.Assignment
	if err := json.Unmarshal(result.Body.Bytes(), &assignment); err != nil {
		t.Fatalf("decode assignment: %v", err)
	}
	if assignment.AssignmentID == "" || assignment.AttemptID == "" {
		t.Fatalf("assignment has no identity: %+v", assignment)
	}
	// The fencing token and version are what every later write is judged
	// against, so an assignment handed out without them is unusable.
	if assignment.FencingToken <= 0 || assignment.AssignmentVersion <= 0 {
		t.Fatalf("assignment is not fenced: %+v", assignment)
	}
	if assignment.Task.Name != "format-check" {
		t.Fatalf("assignment carries the wrong task: %+v", assignment.Task)
	}
	if assignment.Source.Commit != work.commit {
		t.Fatalf("assignment source commit %q, want %q", assignment.Source.Commit, work.commit)
	}
	// The deadline is absolute and the remaining budget is relative; an agent
	// uses whichever its clock can trust, so both have to be real.
	if assignment.DeadlineAt.IsZero() || !assignment.DeadlineAt.After(time.Now().UTC()) {
		t.Fatalf("assignment deadline is not ahead: %v", assignment.DeadlineAt)
	}
	if assignment.RemainingMS <= 0 {
		t.Fatalf("assignment has no remaining budget: %d", assignment.RemainingMS)
	}

	// A second claim BEFORE the first is acknowledged hands back the SAME
	// grant rather than minting a second attempt. That is what makes a claim
	// safe to retry: an agent whose response was lost in transit asks again
	// and gets the work it already holds, instead of the run growing a
	// duplicate attempt nobody will ever acknowledge.
	second := work.claimOne(t)
	if second.AssignmentID != assignment.AssignmentID || second.AttemptID != assignment.AttemptID {
		t.Fatalf("a retried claim minted a second attempt: %s/%s then %s/%s",
			assignment.AssignmentID, assignment.AttemptID, second.AssignmentID, second.AttemptID)
	}
	if second.FencingToken != assignment.FencingToken || second.AssignmentVersion != assignment.AssignmentVersion {
		t.Fatalf("a retried claim moved the fence: %d/%d then %d/%d",
			assignment.AssignmentVersion, assignment.FencingToken,
			second.AssignmentVersion, second.FencingToken)
	}
}

func TestIntegrationABeatOnALiveGrantIsAnsweredOnThatGrantsTerms(t *testing.T) {
	work := claimingPlane(t, true)
	assignment := work.claimOne(t)
	work.acknowledge(t, assignment)

	for _, phase := range []string{"executing", "finishing"} {
		t.Run(phase, func(t *testing.T) {
			result := work.agentKnock(t, "/v1/agents/me/heartbeat", protocol.Heartbeat{
				SchemaVersion:     protocol.Version,
				AssignmentID:      assignment.AssignmentID,
				AttemptID:         assignment.AttemptID,
				AssignmentVersion: assignment.AssignmentVersion,
				FencingToken:      assignment.FencingToken,
				Phase:             phase,
				HostFacts:         work.facts,
			})
			if result.Code != http.StatusOK {
				t.Fatalf("beat answered %d: %s", result.Code, result.Body.String())
			}
			var response protocol.HeartbeatResponse
			if err := json.Unmarshal(result.Body.Bytes(), &response); err != nil {
				t.Fatalf("decode beat: %v", err)
			}
			// The agent checks the answer against the grant it holds before
			// acting on it, so an answer that does not satisfy ValidateFor
			// would be discarded by a correct agent and the control intent
			// lost in silence.
			if err := response.ValidateFor(assignment); err != nil {
				t.Fatalf("beat answer does not bind to the grant: %v (%+v)", err, response)
			}
			// Nothing has asked this attempt to stop.
			if response.Cancel || response.Yield {
				t.Fatalf("an undisturbed attempt was told to stop: %+v", response)
			}
		})
	}
}

func TestIntegrationABeatCarryingAnotherGrantsFenceIsRefused(t *testing.T) {
	work := claimingPlane(t, true)
	assignment := work.claimOne(t)
	work.acknowledge(t, assignment)

	// A fencing token that is not this grant's is the signature of a
	// superseded worker still beating against work it no longer holds. It
	// must not be answered as if it were live, or the stale worker keeps
	// believing it owns the attempt.
	result := work.agentKnock(t, "/v1/agents/me/heartbeat", protocol.Heartbeat{
		SchemaVersion:     protocol.Version,
		AssignmentID:      assignment.AssignmentID,
		AttemptID:         assignment.AttemptID,
		AssignmentVersion: assignment.AssignmentVersion,
		FencingToken:      assignment.FencingToken + 1,
		Phase:             "executing",
		HostFacts:         work.facts,
	})
	if result.Code == http.StatusOK {
		t.Fatalf("a beat on another grant's fence was answered as live: %s", result.Body.String())
	}
	if result.Code < 400 {
		t.Fatalf("a stale beat answered %d, want a refusal", result.Code)
	}
}
