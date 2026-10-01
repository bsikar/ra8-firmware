//go:build integration

package server

import (
	"bytes"
	"crypto/sha256"
	"crypto/tls"
	"crypto/x509"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"net/url"
	"os"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/migrations"
	"github.com/jackc/pgx/v5/pgxpool"
)

// backend is one reservation's Terraform state endpoint, already authorized.
type backend struct {
	plane       *Server
	reservation string
	certificate *x509.Certificate
}

// terraformBackend reserves a runner VM and grants a certificate the
// terraform_state permission over its repository, which is the whole fixture
// this endpoint needs: it looks the repository up from the reservation and
// authorizes against that, so neither half can be skipped.
//
// The store opens with a Terraform state key, because every read and write
// here is sealed at rest; a store opened without one refuses with "Terraform
// state encryption is not configured" before touching a row.
func terraformBackend(t *testing.T) *backend {
	t.Helper()
	dsn := os.Getenv("RA8CI_TEST_PG_DSN")
	if dsn == "" {
		t.Fatal("RA8CI_TEST_PG_DSN is required for integration tests")
	}
	config, err := pgxpool.ParseConfig(dsn)
	if err != nil || config.ConnConfig.Host != "127.0.0.1" || config.ConnConfig.Database != "ra8ci_test" {
		t.Fatal("RA8CI_TEST_PG_DSN must identify disposable loopback ra8ci_test")
	}
	ctx := t.Context()
	owner, err := pgxpool.New(ctx, dsn)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(owner.Close)
	if err := migrations.Apply(ctx, owner); err != nil {
		t.Fatal(err)
	}
	grantRuntimeRole(t, owner)

	// Every uniquely-constrained column is keyed to this moment. The suite has
	// to be runnable more than once against one database, and runner_vms is
	// unique on (node, vmid) and on (scale_set_id, workflow_run_id,
	// workflow_attempt, job_id).
	unique := time.Now().UnixNano()
	reservation, err := store.NewID()
	if err != nil {
		t.Fatal(err)
	}
	creation, err := store.NewID()
	if err != nil {
		t.Fatal(err)
	}
	_, err = owner.Exec(ctx, `INSERT INTO runner_vms
		(id,scale_set_id,job_id,runner_request_id,workflow_run_id,workflow_attempt,
		 repository,workflow_ref,commit_sha,vmid,node,pool,storage,vm_name,
		 template_vmid,template_name,creation_operation_id,state,unclaimed_deadline)
		VALUES ($1,$2,$3,$4,$5,1,'bsikar/ra8-firmware','.github/workflows/ci.yml',
		 $6,$7,'ra8-node-1','ra8-pool','local-lvm',$8,100,'ra8-template',$9,'reserved',
		 clock_timestamp()+interval '1 hour')`,
		reservation, unique%1000000+1, fmt.Sprintf("job-%d", unique), unique%1000000000+1,
		unique%1000000000+1, strings.Repeat("a", 40), int(unique%40000)+9000,
		fmt.Sprintf("ra8-lab-%d", unique), creation)
	if err != nil {
		t.Fatal(err)
	}

	certificate := testClientCertificate(t)
	fingerprint := sha256.Sum256(certificate.Raw)
	principal, err := store.NewID()
	if err != nil {
		t.Fatal(err)
	}
	_, err = owner.Exec(ctx, `INSERT INTO api_principals
		(cert_sha256, principal_id, kind, expires_at) VALUES ($1,$2,'human',clock_timestamp()+interval '1 hour')`,
		hex.EncodeToString(fingerprint[:]), principal)
	if err != nil {
		t.Fatal(err)
	}
	_, err = owner.Exec(ctx, `INSERT INTO api_grants (principal_id, repository, role)
		VALUES ($1,'bsikar/ra8-firmware','terraform_state')`, principal)
	if err != nil {
		t.Fatal(err)
	}

	runtimeURL, err := url.Parse(dsn)
	if err != nil {
		t.Fatal(err)
	}
	runtimeURL.User = url.UserPassword("ra8ci_server_runtime_test", "ra8ci_server_runtime_test_only")
	key := make([]byte, 32)
	for i := range key {
		key[i] = byte(i + 1)
	}
	st, err := store.OpenWithTerraformStateKey(ctx, runtimeURL.String(), key)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(st.Close)
	cat, err := catalog.Load()
	if err != nil {
		t.Fatal(err)
	}
	plane, err := New(st, cat)
	if err != nil {
		t.Fatal(err)
	}
	return &backend{plane: plane, reservation: reservation, certificate: certificate}
}

// grantRuntimeRole mints the least-privileged role the store will open as.
// store.Open refuses a role that can mutate the append-only audit trail, so
// connecting as the owner fails before any door is asked anything.
func grantRuntimeRole(t *testing.T, owner *pgxpool.Pool) {
	t.Helper()
	ctx := t.Context()
	roleTx, err := owner.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer func() { _ = roleTx.Rollback(ctx) }()
	if _, err := roleTx.Exec(ctx, `SELECT pg_advisory_xact_lock(72628802)`); err != nil {
		t.Fatal(err)
	}
	if _, err := roleTx.Exec(ctx, `DO $$ BEGIN
		IF NOT EXISTS (SELECT 1 FROM pg_roles WHERE rolname='ra8ci_server_runtime_test') THEN
			CREATE ROLE ra8ci_server_runtime_test LOGIN PASSWORD 'ra8ci_server_runtime_test_only';
		END IF;
	END $$`); err != nil {
		t.Fatal(err)
	}
	if _, err := roleTx.Exec(ctx, `GRANT CONNECT ON DATABASE ra8ci_test TO ra8ci_server_runtime_test;
		GRANT USAGE ON SCHEMA public TO ra8ci_server_runtime_test;
		GRANT SELECT,INSERT,UPDATE ON ALL TABLES IN SCHEMA public TO ra8ci_server_runtime_test;
		REVOKE INSERT,UPDATE ON schema_migrations,board_fixture_profiles FROM ra8ci_server_runtime_test;
		REVOKE INSERT,UPDATE,DELETE,TRUNCATE ON api_principals,api_grants,agents,
			board_fixture_profiles,schema_migrations FROM ra8ci_server_runtime_test;
		REVOKE UPDATE ON audit,board_events,run_events,local_runs,local_run_steps,hil_observations FROM ra8ci_server_runtime_test`); err != nil {
		t.Fatal(err)
	}
	if err := roleTx.Commit(ctx); err != nil {
		t.Fatal(err)
	}
}

// call asks the backend one request as the authorized client.
func (b *backend) call(t *testing.T, method, query string, body []byte) *httptest.ResponseRecorder {
	t.Helper()
	return b.callAs(t, method, query, body, b.certificate)
}

func (b *backend) callAs(t *testing.T, method, query string, body []byte, certificate *x509.Certificate) *httptest.ResponseRecorder {
	t.Helper()
	target := "/v1/terraform/runner-states/" + b.reservation + query
	request := httptest.NewRequest(method, target, bytes.NewReader(body))
	if certificate != nil {
		request.TLS = &tls.ConnectionState{
			PeerCertificates: []*x509.Certificate{certificate},
			VerifiedChains:   [][]*x509.Certificate{{certificate}},
		}
	}
	recorder := httptest.NewRecorder()
	b.plane.Handler().ServeHTTP(recorder, request)
	return recorder
}

// terraformLock is a lock payload Terraform itself would send.
func terraformLock(t *testing.T) (string, []byte) {
	t.Helper()
	id, err := store.NewID()
	if err != nil {
		t.Fatal(err)
	}
	raw, err := json.Marshal(map[string]string{
		"ID": id, "Operation": "OperationTypeApply", "Who": "ra8ci@test",
		"Version": "1.9.5", "Created": "2026-01-01T00:00:00Z", "Path": "ra8ci/runner",
	})
	if err != nil {
		t.Fatal(err)
	}
	return id, raw
}

// terraformState is a state file Terraform itself would push, at the given serial.
func terraformState(serial int64) []byte {
	return []byte(fmt.Sprintf(`{"version":4,"terraform_version":"1.9.5","serial":%d,`+
		`"lineage":"4f1b2c3d-1a2b-4c3d-8e9f-0a1b2c3d4e5f","outputs":{},"resources":[]}`, serial))
}

// The backend's whole working cycle, in the order Terraform drives it: lock,
// push, read back, delete, unlock. Pinned as one test because each step is
// the previous step's precondition, and a read that did not follow a write
// proves nothing about what was stored.
func TestIntegrationTerraformBackendServesALockedApplyCycle(t *testing.T) {
	b := terraformBackend(t)

	if got := b.call(t, http.MethodGet, "", nil); got.Code != http.StatusNotFound {
		t.Fatalf("an untouched reservation answered %d, want 404", got.Code)
	}
	lockID, lock := terraformLock(t)
	if got := b.call(t, terraformLockMethod, "", lock); got.Code != http.StatusOK {
		t.Fatalf("lock answered %d, want 200: %s", got.Code, got.Body.String())
	}
	state := terraformState(1)
	if got := b.call(t, http.MethodPost, "?ID="+lockID, state); got.Code != http.StatusOK {
		t.Fatalf("push answered %d, want 200: %s", got.Code, got.Body.String())
	}

	read := b.call(t, http.MethodGet, "", nil)
	if read.Code != http.StatusOK {
		t.Fatalf("read answered %d, want 200: %s", read.Code, read.Body.String())
	}
	if !bytes.Equal(read.Body.Bytes(), state) {
		t.Fatalf("read back %s, want the pushed state", read.Body.String())
	}
	if got := read.Header().Get("Content-Type"); got != "application/json" {
		t.Fatalf("got content type %q, want application/json", got)
	}
	// State is a secret at rest and must not be cached on the way out.
	if got := read.Header().Get("Cache-Control"); got != "no-store" {
		t.Fatalf("got cache control %q, want no-store", got)
	}

	if got := b.call(t, http.MethodDelete, "?ID="+lockID, nil); got.Code != http.StatusOK {
		t.Fatalf("delete answered %d, want 200: %s", got.Code, got.Body.String())
	}
	if got := b.call(t, http.MethodGet, "", nil); got.Code != http.StatusNotFound {
		t.Fatalf("a deleted state answered %d, want 404", got.Code)
	}
	if got := b.call(t, terraformUnlockMethod, "", lock); got.Code != http.StatusOK {
		t.Fatalf("unlock answered %d, want 200: %s", got.Code, got.Body.String())
	}
}

// A push with nobody holding the lock is the race the backend exists to stop.
func TestIntegrationTerraformBackendRefusesAPushWithoutTheLock(t *testing.T) {
	b := terraformBackend(t)
	lockID, _ := terraformLock(t)

	got := b.call(t, http.MethodPost, "?ID="+lockID, terraformState(1))
	if got.Code != http.StatusConflict {
		t.Fatalf("an unlocked push answered %d, want 409: %s", got.Code, got.Body.String())
	}
	if got := b.call(t, http.MethodGet, "", nil); got.Code != http.StatusNotFound {
		t.Fatal("the refused push left state behind")
	}
}

// A push under somebody else's lock is refused too: holding A lock is not
// holding THE lock.
func TestIntegrationTerraformBackendRefusesAPushUnderAnotherLock(t *testing.T) {
	b := terraformBackend(t)
	_, held := terraformLock(t)
	if got := b.call(t, terraformLockMethod, "", held); got.Code != http.StatusOK {
		t.Fatalf("lock answered %d, want 200", got.Code)
	}
	other, _ := terraformLock(t)

	got := b.call(t, http.MethodPost, "?ID="+other, terraformState(1))
	if got.Code != http.StatusConflict {
		t.Fatalf("a push under a foreign lock answered %d, want 409: %s", got.Code, got.Body.String())
	}
}

// Terraform reads the current holder off the 423 body to tell the operator who
// is applying, so the refusal has to carry the lock info rather than only its
// status.
func TestIntegrationTerraformBackendReportsTheHolderToASecondLock(t *testing.T) {
	b := terraformBackend(t)
	_, first := terraformLock(t)
	if got := b.call(t, terraformLockMethod, "", first); got.Code != http.StatusOK {
		t.Fatalf("first lock answered %d, want 200", got.Code)
	}
	_, second := terraformLock(t)

	got := b.call(t, terraformLockMethod, "", second)
	if got.Code != http.StatusLocked {
		t.Fatalf("a second lock answered %d, want 423: %s", got.Code, got.Body.String())
	}
	var held map[string]any
	if err := json.Unmarshal(got.Body.Bytes(), &held); err != nil {
		t.Fatalf("the 423 body is not the held lock: %q", got.Body.String())
	}
	var want map[string]any
	if err := json.Unmarshal(first, &want); err != nil {
		t.Fatal(err)
	}
	if held["ID"] != want["ID"] || held["Who"] != want["Who"] {
		t.Fatalf("the 423 named %v, want the first holder %v", held["ID"], want["ID"])
	}
}

// Re-locking with the same ID is how Terraform retries a dropped response, so
// it must be idempotent rather than a self-inflicted 423.
func TestIntegrationTerraformBackendTreatsARepeatedLockAsHeld(t *testing.T) {
	b := terraformBackend(t)
	_, lock := terraformLock(t)
	if got := b.call(t, terraformLockMethod, "", lock); got.Code != http.StatusOK {
		t.Fatalf("lock answered %d, want 200", got.Code)
	}
	if got := b.call(t, terraformLockMethod, "", lock); got.Code != http.StatusOK {
		t.Fatalf("the same lock again answered %d, want 200: %s", got.Code, got.Body.String())
	}
}

// Releasing somebody else's lock must fail, or a stalled apply could be
// unlocked out from under itself.
func TestIntegrationTerraformBackendRefusesAForeignUnlock(t *testing.T) {
	b := terraformBackend(t)
	_, held := terraformLock(t)
	if got := b.call(t, terraformLockMethod, "", held); got.Code != http.StatusOK {
		t.Fatalf("lock answered %d, want 200", got.Code)
	}
	_, foreign := terraformLock(t)

	got := b.call(t, terraformUnlockMethod, "", foreign)
	if got.Code != http.StatusConflict {
		t.Fatalf("a foreign unlock answered %d, want 409: %s", got.Code, got.Body.String())
	}
	// The real holder is still holding, which is the point of refusing.
	_, another := terraformLock(t)
	if got := b.call(t, terraformLockMethod, "", another); got.Code != http.StatusLocked {
		t.Fatalf("after the refused unlock a new lock answered %d, want 423", got.Code)
	}
}

// Serial is Terraform's fence against a stale writer overwriting a newer
// state. Going backwards is refused; replaying the identical serial is not,
// because that is a retried request rather than a conflicting one.
func TestIntegrationTerraformBackendHoldsTheSerialFence(t *testing.T) {
	b := terraformBackend(t)
	lockID, lock := terraformLock(t)
	if got := b.call(t, terraformLockMethod, "", lock); got.Code != http.StatusOK {
		t.Fatalf("lock answered %d, want 200", got.Code)
	}
	if got := b.call(t, http.MethodPost, "?ID="+lockID, terraformState(7)); got.Code != http.StatusOK {
		t.Fatalf("serial 7 answered %d, want 200: %s", got.Code, got.Body.String())
	}
	if got := b.call(t, http.MethodPost, "?ID="+lockID, terraformState(6)); got.Code != http.StatusConflict {
		t.Fatalf("serial 6 after 7 answered %d, want 409", got.Code)
	}
	if got := b.call(t, http.MethodPost, "?ID="+lockID, terraformState(7)); got.Code != http.StatusOK {
		t.Fatalf("replaying serial 7 answered %d, want 200", got.Code)
	}
	if got := b.call(t, http.MethodPost, "?ID="+lockID, terraformState(8)); got.Code != http.StatusOK {
		t.Fatalf("serial 8 answered %d, want 200", got.Code)
	}
	read := b.call(t, http.MethodGet, "", nil)
	if !bytes.Equal(read.Body.Bytes(), terraformState(8)) {
		t.Fatalf("the backend settled at %s, want serial 8", read.Body.String())
	}
}

// A body that is not a Terraform state envelope is refused as a bad request,
// not stored and not reported as a conflict.
func TestIntegrationTerraformBackendRefusesABodyThatIsNotState(t *testing.T) {
	b := terraformBackend(t)
	lockID, lock := terraformLock(t)
	if got := b.call(t, terraformLockMethod, "", lock); got.Code != http.StatusOK {
		t.Fatalf("lock answered %d, want 200", got.Code)
	}
	for _, body := range []string{
		`{"version":3,"terraform_version":"1.9.5","serial":1,"lineage":"4f1b2c3d-1a2b-4c3d-8e9f-0a1b2c3d4e5f"}`,
		`{"version":4,"terraform_version":"1.9.5","serial":1,"lineage":"not-a-lineage"}`,
		`{"version":4,"serial":1,"lineage":"4f1b2c3d-1a2b-4c3d-8e9f-0a1b2c3d4e5f"}`,
		`not json at all`,
		``,
	} {
		got := b.call(t, http.MethodPost, "?ID="+lockID, []byte(body))
		if got.Code != http.StatusBadRequest {
			t.Fatalf("body %q answered %d, want 400", body, got.Code)
		}
	}
}

// The route is registered without a method so LOCK and UNLOCK can reach it,
// which means the handler itself owes an unsupported method the Allow header.
func TestIntegrationTerraformBackendAnswersAnUnsupportedMethodWithAllow(t *testing.T) {
	b := terraformBackend(t)

	got := b.call(t, http.MethodPatch, "", nil)
	if got.Code != http.StatusMethodNotAllowed {
		t.Fatalf("PATCH answered %d, want 405", got.Code)
	}
	allow := got.Header().Get("Allow")
	for _, method := range terraformStateMethods {
		if !strings.Contains(allow, method) {
			t.Fatalf("Allow %q omits %s", allow, method)
		}
	}
}

// A malformed reservation is refused as a bad request before any lookup, and
// an unknown one as not found. Neither reaches authorization, so neither can
// report whether a grant would have matched.
func TestIntegrationTerraformBackendRefusesAReservationItCannotPlace(t *testing.T) {
	b := terraformBackend(t)

	b.reservation = "not-a-uuid"
	if got := b.call(t, http.MethodGet, "", nil); got.Code != http.StatusBadRequest {
		t.Fatalf("a malformed reservation answered %d, want 400", got.Code)
	}
	absent, err := store.NewID()
	if err != nil {
		t.Fatal(err)
	}
	b.reservation = absent
	if got := b.call(t, http.MethodGet, "", nil); got.Code != http.StatusNotFound {
		t.Fatalf("an unknown reservation answered %d, want 404", got.Code)
	}
	// Even with no certificate at all, which is what "before authorization"
	// means: the reservation is judged on its own.
	if got := b.callAs(t, http.MethodGet, "", nil, nil); got.Code != http.StatusNotFound {
		t.Fatalf("an unknown reservation without a certificate answered %d, want 404", got.Code)
	}
}

// A caller without a verified certificate is refused, and refused with the
// same STATUS whether or not the reservation is real.
//
// The reservation is looked up before the caller is authorized, because
// authorization is scoped to the repository the reservation names, so the two
// refusals are reached by different paths and could easily drift apart in
// status. They must not: an unauthenticated 403 on a real reservation beside a
// 404 on an absent one would let a stranger confirm a reservation ID by its
// status code alone.
//
// Only the status is pinned here. The two paths do still carry different
// problem codes, "denied" against "not_found", which is recorded in the lane
// ledger as an observation rather than asserted either way: reservation IDs
// are UUID v7 and cannot be walked, so the distinction is not reachable
// without already holding the ID, and whether to flatten the wording is a
// design call rather than something a test should quietly decide.
func TestIntegrationTerraformBackendRefusesAStrangerWithTheSameStatus(t *testing.T) {
	b := terraformBackend(t)
	absent, err := store.NewID()
	if err != nil {
		t.Fatal(err)
	}

	onReal := b.callAs(t, http.MethodGet, "", nil, nil)
	if onReal.Code != http.StatusNotFound {
		t.Fatalf("an unauthenticated read of a real reservation answered %d, want 404: %s",
			onReal.Code, onReal.Body.String())
	}
	b.reservation = absent
	onAbsent := b.callAs(t, http.MethodGet, "", nil, nil)
	if onAbsent.Code != onReal.Code {
		t.Fatalf("real answered %d and absent answered %d: a stranger can tell them apart by status",
			onReal.Code, onAbsent.Code)
	}
	// Neither refusal may carry any of the state itself.
	for _, refusal := range []*httptest.ResponseRecorder{onReal, onAbsent} {
		if strings.Contains(refusal.Body.String(), "lineage") || strings.Contains(refusal.Body.String(), "terraform_version") {
			t.Fatalf("a refusal leaked state: %s", refusal.Body.String())
		}
	}
}
