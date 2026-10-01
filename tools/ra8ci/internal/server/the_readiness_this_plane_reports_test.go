//go:build integration

package server

import (
	"context"
	"encoding/json"
	"errors"
	"net/http"
	"net/http/httptest"
	"net/url"
	"os"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/migrations"
	"github.com/jackc/pgx/v5/pgxpool"
)

// readinessPlane builds a server over a real store, which is what the
// readiness door needs: every branch of it turns on whether the database and
// the append-only audit trail can actually be written, so a test double would
// only restate the double. The unit tests beside this file already pin
// checkReadiness itself; what is pinned here is the HTTP door over it.
//
// The store opens as the least-privileged runtime role rather than the owner.
// store.Open refuses a role that can mutate the audit trail, so connecting as
// the owner fails with "runtime role can mutate append-only audit" before any
// door is asked anything.
//
// The store is handed back rather than closed here, because several of these
// tests close it on purpose to see what the door says once the database is
// gone.
func readinessPlane(t *testing.T, checks ...func(context.Context) error) (*Server, *store.Store) {
	t.Helper()
	dsn := os.Getenv("RA8CI_TEST_PG_DSN")
	if dsn == "" {
		t.Fatal("RA8CI_TEST_PG_DSN is required for integration tests")
	}
	config, err := pgxpool.ParseConfig(dsn)
	if err != nil || config.ConnConfig.Host != "127.0.0.1" || config.ConnConfig.Database != "ra8ci_test" {
		t.Fatal("RA8CI_TEST_PG_DSN must identify disposable loopback ra8ci_test")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	owner, err := pgxpool.New(ctx, dsn)
	if err != nil {
		t.Fatal(err)
	}
	defer owner.Close()
	if err := migrations.Apply(ctx, owner); err != nil {
		t.Fatal(err)
	}
	roleTx, err := owner.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer func() { _ = roleTx.Rollback(ctx) }()
	// The same advisory lock the other integration tests take, so two of them
	// minting this shared role at once cannot collide.
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
	runtimeURL, err := url.Parse(dsn)
	if err != nil {
		t.Fatal(err)
	}
	runtimeURL.User = url.UserPassword("ra8ci_server_runtime_test", "ra8ci_server_runtime_test_only")
	st, err := store.Open(ctx, runtimeURL.String())
	if err != nil {
		t.Fatal(err)
	}
	cat, err := catalog.Load()
	if err != nil {
		st.Close()
		t.Fatal(err)
	}
	plane, err := NewWithOptions(st, cat, nil, "", checks...)
	if err != nil {
		st.Close()
		t.Fatal(err)
	}
	return plane, st
}

// probe asks one health door and hands back what it answered.
func probe(t *testing.T, plane *Server, path string) (int, map[string]any) {
	t.Helper()
	recorder := httptest.NewRecorder()
	plane.Handler().ServeHTTP(recorder, httptest.NewRequest(http.MethodGet, path, nil))
	body := map[string]any{}
	if recorder.Body.Len() > 0 {
		if err := json.Unmarshal(recorder.Body.Bytes(), &body); err != nil {
			t.Fatalf("%s answered %d with a body that is not JSON: %q", path, recorder.Code, recorder.Body.String())
		}
	}
	return recorder.Code, body
}

// A ready plane states the schema version it is actually serving. An
// orchestrator reads that number to tell a rolled-back deployment from a
// current one, so answering ready without it would be answering nothing.
func TestIntegrationReadinessStatesTheSchemaItServes(t *testing.T) {
	plane, st := readinessPlane(t)
	defer st.Close()

	code, body := probe(t, plane, "/health/ready")
	if code != http.StatusOK {
		t.Fatalf("a writable plane answered %d, want 200: %v", code, body)
	}
	if body["status"] != "ready" {
		t.Fatalf("got status %v, want ready", body["status"])
	}
	schema, ok := body["schema"].(float64)
	if !ok || int(schema) != migrations.CurrentVersion() {
		t.Fatalf("got schema %v, want %d", body["schema"], migrations.CurrentVersion())
	}
}

// Readiness is not liveness. A configured dependency being down must hold the
// plane out of the load balancer without claiming the process is dead, or an
// orchestrator restarts a server whose only problem is somewhere else.
func TestIntegrationReadinessRefusesWhenAConfiguredDependencyIsDown(t *testing.T) {
	down := errors.New("the thing this plane waits on is not up")
	plane, st := readinessPlane(t, func(context.Context) error { return down })
	defer st.Close()

	code, body := probe(t, plane, "/health/ready")
	if code != http.StatusServiceUnavailable {
		t.Fatalf("a failed dependency answered %d, want 503: %v", code, body)
	}
	if body["detail"] != "a configured readiness dependency is unavailable" {
		t.Fatalf("got detail %q, want the dependency wording", body["detail"])
	}
	if body["retryable"] != true {
		t.Fatal("a dependency that may come back must be reported retryable")
	}
	// The dependency's own error text must not reach the caller: it is written
	// by whatever configured the check and is not a stranger's to read.
	if body["detail"] == down.Error() {
		t.Fatal("the dependency's own error text reached the caller")
	}
}

// With the database gone the door must say so rather than hang or panic, and
// it must name the DATABASE: that wording is what tells an operator where to
// look first.
func TestIntegrationReadinessRefusesWhenTheDatabaseIsGone(t *testing.T) {
	plane, st := readinessPlane(t)
	st.Close()

	code, body := probe(t, plane, "/health/ready")
	if code != http.StatusServiceUnavailable {
		t.Fatalf("a closed store answered %d, want 503: %v", code, body)
	}
	if body["detail"] != "database or audit is not writable" {
		t.Fatalf("got detail %q, want the database wording", body["detail"])
	}
	if body["retryable"] != true {
		t.Fatal("a database that may come back must be reported retryable")
	}
}

// The store is judged ahead of the configured checks, so an operator facing
// both failures is pointed at the database rather than at a dependency that
// cannot be reached until the database is back.
func TestIntegrationReadinessNamesTheDatabaseAheadOfADependency(t *testing.T) {
	plane, st := readinessPlane(t, func(context.Context) error { return errors.New("also down") })
	st.Close()

	code, body := probe(t, plane, "/health/ready")
	if code != http.StatusServiceUnavailable {
		t.Fatalf("got %d, want 503: %v", code, body)
	}
	if body["detail"] != "database or audit is not writable" {
		t.Fatalf("got detail %q, want the database named first", body["detail"])
	}
}

// Liveness must keep answering while readiness refuses. A process whose
// database blipped is not a process to kill, and conflating the two is how a
// brief outage becomes a restart loop.
func TestIntegrationLivenessAnswersWhileReadinessRefuses(t *testing.T) {
	plane, st := readinessPlane(t)
	st.Close()

	if code, body := probe(t, plane, "/health/ready"); code != http.StatusServiceUnavailable {
		t.Fatalf("readiness answered %d, want 503: %v", code, body)
	}
	code, body := probe(t, plane, "/health/live")
	if code != http.StatusOK {
		t.Fatalf("liveness answered %d with the database down, want 200: %v", code, body)
	}
	if body["status"] != "alive" {
		t.Fatalf("got status %v, want alive", body["status"])
	}
}
