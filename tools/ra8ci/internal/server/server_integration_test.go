//go:build integration

package server

import (
	"bytes"
	"context"
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/sha256"
	"crypto/tls"
	"crypto/x509"
	"encoding/hex"
	"encoding/json"
	"math/big"
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

func TestIntegrationMTLSRunAdmissionAndStatus(t *testing.T) {
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
	defer pool.Close()
	if err := migrations.Apply(ctx, pool); err != nil {
		t.Fatal(err)
	}
	roleTx, err := pool.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer func() { _ = roleTx.Rollback(ctx) }()
	if _, err := roleTx.Exec(ctx, `SELECT pg_advisory_xact_lock(72628802)`); err != nil {
		t.Fatal(err)
	}
	_, err = roleTx.Exec(ctx, `DO $$ BEGIN
		IF NOT EXISTS (SELECT 1 FROM pg_roles WHERE rolname='ra8ci_server_runtime_test') THEN
			CREATE ROLE ra8ci_server_runtime_test LOGIN PASSWORD 'ra8ci_server_runtime_test_only';
		END IF;
	END $$`)
	if err != nil {
		t.Fatal(err)
	}
	_, err = roleTx.Exec(ctx, `GRANT CONNECT ON DATABASE ra8ci_test TO ra8ci_server_runtime_test;
		GRANT USAGE ON SCHEMA public TO ra8ci_server_runtime_test;
		GRANT SELECT,INSERT,UPDATE ON ALL TABLES IN SCHEMA public TO ra8ci_server_runtime_test;
		REVOKE INSERT,UPDATE ON schema_migrations,board_fixture_profiles FROM ra8ci_server_runtime_test;
		REVOKE INSERT,UPDATE,DELETE,TRUNCATE ON api_principals,api_grants,agents,
			board_fixture_profiles,schema_migrations FROM ra8ci_server_runtime_test;
		REVOKE UPDATE ON audit,board_events,run_events,local_runs,local_run_steps FROM ra8ci_server_runtime_test`)
	if err != nil {
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
	defer st.Close()
	cat, err := catalog.Load()
	if err != nil {
		t.Fatal(err)
	}
	server, err := New(st, cat)
	if err != nil {
		t.Fatal(err)
	}
	certificate := testClientCertificate(t)
	fingerprint := sha256.Sum256(certificate.Raw)
	principal, err := store.NewID()
	if err != nil {
		t.Fatal(err)
	}
	_, err = pool.Exec(ctx, `INSERT INTO api_principals
		(cert_sha256, principal_id, kind, expires_at) VALUES ($1,$2,'human',clock_timestamp()+interval '1 hour')`,
		hex.EncodeToString(fingerprint[:]), principal)
	if err != nil {
		t.Fatal(err)
	}
	_, err = pool.Exec(ctx, `INSERT INTO api_grants (principal_id, repository, role)
		VALUES ($1,'bsikar/ra8-firmware','submitter')`, principal)
	if err != nil {
		t.Fatal(err)
	}
	key, err := store.NewID()
	if err != nil {
		t.Fatal(err)
	}
	payload, err := json.Marshal(map[string]any{
		"trigger": "manual",
		"source": map[string]any{
			"repo": "bsikar/ra8-firmware", "branch": "ci/orchestrator",
			"commit": strings.Repeat("a", 40), "snapshot_sha256": strings.Repeat("b", 64),
		},
		"catalog_digest": cat.Digest(),
		"tasks":          []map[string]any{{"key": "check", "name": "format-check", "args": []string{}, "depends_on_keys": []string{}}},
	})
	if err != nil {
		t.Fatal(err)
	}
	makeRequest := func(method, path string, body []byte, withCert bool) *http.Request {
		req := httptest.NewRequest(method, path, bytes.NewReader(body))
		if withCert {
			req.TLS = &tls.ConnectionState{
				PeerCertificates: []*x509.Certificate{certificate},
				VerifiedChains:   [][]*x509.Certificate{{certificate}},
			}
		}
		if method == http.MethodPost {
			req.Header.Set("Content-Type", "application/json")
			req.Header.Set("Idempotency-Key", key)
		}
		return req
	}
	post := httptest.NewRecorder()
	server.Handler().ServeHTTP(post, makeRequest(http.MethodPost, "/v1/runs", payload, true))
	if post.Code != http.StatusCreated {
		t.Fatalf("authenticated admission status %d: %s", post.Code, post.Body.String())
	}
	location := post.Header().Get("Location")
	if !strings.HasPrefix(location, "/v1/runs/") {
		t.Fatalf("missing run location: %q", location)
	}
	replay := httptest.NewRecorder()
	server.Handler().ServeHTTP(replay, makeRequest(http.MethodPost, "/v1/runs", payload, true))
	if replay.Code != http.StatusCreated || replay.Header().Get("Location") != location {
		t.Fatalf("idempotent HTTP replay diverged: %d %s", replay.Code, replay.Body.String())
	}
	read := httptest.NewRecorder()
	server.Handler().ServeHTTP(read, makeRequest(http.MethodGet, location, nil, true))
	if read.Code != http.StatusOK || !strings.Contains(read.Body.String(), `"name":"format-check"`) {
		t.Fatalf("authenticated status failed: %d %s", read.Code, read.Body.String())
	}
	denied := httptest.NewRecorder()
	server.Handler().ServeHTTP(denied, makeRequest(http.MethodGet, location, nil, false))
	if denied.Code != http.StatusNotFound {
		t.Fatalf("unauthenticated status exposed run: %d %s", denied.Code, denied.Body.String())
	}
	var denialCount int
	if err := pool.QueryRow(ctx, "SELECT COUNT(*) FROM audit WHERE action='run.read' AND outcome='denied' AND target_id=$1", strings.TrimPrefix(location, "/v1/runs/")).Scan(&denialCount); err != nil || denialCount != 1 {
		t.Fatalf("denial audit missing: count %d error %v", denialCount, err)
	}
	reportPath := "/v1/reports/slow?repository=bsikar%2Fra8-firmware&window_seconds=3600&limit=10"
	reportOK := httptest.NewRecorder()
	server.Handler().ServeHTTP(reportOK, makeRequest(http.MethodGet, reportPath, nil, true))
	if reportOK.Code != http.StatusOK || !strings.Contains(reportOK.Body.String(), `"window_seconds":3600`) {
		t.Fatalf("authenticated slow report failed: %d %s", reportOK.Code, reportOK.Body.String())
	}
	reportDenied := httptest.NewRecorder()
	server.Handler().ServeHTTP(reportDenied, makeRequest(http.MethodGet, reportPath, nil, false))
	if reportDenied.Code != http.StatusNotFound {
		t.Fatalf("unauthenticated slow report exposed data: %d %s", reportDenied.Code, reportDenied.Body.String())
	}
	reportInvalid := httptest.NewRecorder()
	server.Handler().ServeHTTP(reportInvalid, makeRequest(http.MethodGet,
		"/v1/reports/slow?repository=bsikar%2Fra8-firmware&window_seconds=31536001&limit=10", nil, true))
	if reportInvalid.Code != http.StatusBadRequest {
		t.Fatalf("unbounded slow report accepted: %d %s", reportInvalid.Code, reportInvalid.Body.String())
	}
	cancelPath := location + "/cancel"
	cancelResponse := httptest.NewRecorder()
	server.Handler().ServeHTTP(cancelResponse, makeRequest(http.MethodPost, cancelPath, nil, true))
	if cancelResponse.Code != http.StatusOK || !strings.Contains(cancelResponse.Body.String(), `"cancel_requested_at"`) {
		t.Fatalf("authorized cancellation failed: %d %s", cancelResponse.Code, cancelResponse.Body.String())
	}
	cancelReplay := httptest.NewRecorder()
	server.Handler().ServeHTTP(cancelReplay, makeRequest(http.MethodPost, cancelPath, nil, true))
	if cancelReplay.Code != http.StatusOK {
		t.Fatalf("idempotent cancellation failed: %d %s", cancelReplay.Code, cancelReplay.Body.String())
	}
	cancelDenied := httptest.NewRecorder()
	server.Handler().ServeHTTP(cancelDenied, makeRequest(http.MethodPost, cancelPath, nil, false))
	if cancelDenied.Code != http.StatusNotFound {
		t.Fatalf("unauthenticated cancellation was not hidden: %d %s", cancelDenied.Code, cancelDenied.Body.String())
	}
	events := httptest.NewRecorder()
	server.Handler().ServeHTTP(events, makeRequest(http.MethodGet, location+"/events?after=0&limit=1", nil, true))
	if events.Code != http.StatusOK {
		t.Fatalf("authenticated run events failed: %d %s", events.Code, events.Body.String())
	}
	var eventPage store.RunEventPage
	if err := json.Unmarshal(events.Body.Bytes(), &eventPage); err != nil || eventPage.RunID != strings.TrimPrefix(location, "/v1/runs/") || len(eventPage.Events) != 1 || !eventPage.HasMore || eventPage.NextAfter != 1 {
		t.Fatalf("run event cursor page is invalid: %+v error %v", eventPage, err)
	}
	eventsDenied := httptest.NewRecorder()
	server.Handler().ServeHTTP(eventsDenied, makeRequest(http.MethodGet, location+"/events", nil, false))
	if eventsDenied.Code != http.StatusNotFound {
		t.Fatalf("unauthenticated run events exposed data: %d %s", eventsDenied.Code, eventsDenied.Body.String())
	}
	eventsInvalid := httptest.NewRecorder()
	server.Handler().ServeHTTP(eventsInvalid, makeRequest(http.MethodGet, location+"/events?limit=51", nil, true))
	if eventsInvalid.Code != http.StatusBadRequest {
		t.Fatalf("unbounded run events page accepted: %d %s", eventsInvalid.Code, eventsInvalid.Body.String())
	}
}

func testClientCertificate(t *testing.T) *x509.Certificate {
	t.Helper()
	key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	template := &x509.Certificate{
		SerialNumber: big.NewInt(1), NotBefore: time.Now().Add(-time.Minute),
		NotAfter: time.Now().Add(time.Hour), KeyUsage: x509.KeyUsageDigitalSignature,
		ExtKeyUsage: []x509.ExtKeyUsage{x509.ExtKeyUsageClientAuth},
	}
	der, err := x509.CreateCertificate(rand.Reader, template, template, &key.PublicKey, key)
	if err != nil {
		t.Fatal(err)
	}
	cert, err := x509.ParseCertificate(der)
	if err != nil {
		t.Fatal(err)
	}
	return cert
}
