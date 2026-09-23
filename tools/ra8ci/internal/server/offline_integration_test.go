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
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/migrations"
	"github.com/jackc/pgx/v5/pgxpool"
)

func TestIntegrationOfflineHTTPIngestNeverQueuesCI(t *testing.T) {
	dsn := os.Getenv("RA8CI_TEST_PG_DSN")
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
	if _, err := roleTx.Exec(ctx, "SELECT pg_advisory_xact_lock(72628802)"); err != nil {
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
	control, err := New(st, cat)
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
		(cert_sha256,principal_id,kind,expires_at) VALUES ($1,$2,'human',clock_timestamp()+interval '1 hour')`,
		hex.EncodeToString(fingerprint[:]), principal)
	if err != nil {
		t.Fatal(err)
	}
	_, err = owner.Exec(ctx, `INSERT INTO api_grants (principal_id,repository,role)
		VALUES ($1,'bsikar/ra8-firmware','submitter')`, principal)
	if err != nil {
		t.Fatal(err)
	}
	entry, _ := offlineTestEntry(t)
	entry.ID = "b9cde0485e9740d9aa228499196e7d43"
	requestBody, err := json.Marshal(entry)
	if err != nil {
		t.Fatal(err)
	}
	makeRequest := func(body []byte, authenticated bool) *http.Request {
		req := httptest.NewRequest(http.MethodPost, "/v1/local-runs/sync", bytes.NewReader(body))
		req.Header.Set("Content-Type", "application/json")
		if authenticated {
			req.TLS = &tls.ConnectionState{PeerCertificates: []*x509.Certificate{certificate},
				VerifiedChains: [][]*x509.Certificate{{certificate}}}
		}
		return req
	}
	first := httptest.NewRecorder()
	control.Handler().ServeHTTP(first, makeRequest(requestBody, true))
	if first.Code != http.StatusOK {
		t.Fatalf("offline upload: %d %s", first.Code, first.Body.String())
	}
	var receipt store.LocalRunReceipt
	if err := json.Unmarshal(first.Body.Bytes(), &receipt); err != nil || !store.ValidID(receipt.LocalRunID) {
		t.Fatalf("offline receipt: %+v %v", receipt, err)
	}
	control.catalog = nil // committed replay must not need today's catalog
	replay := httptest.NewRecorder()
	control.Handler().ServeHTTP(replay, makeRequest(requestBody, true))
	if replay.Code != http.StatusOK || replay.Body.String() != first.Body.String() {
		t.Fatalf("lost-response replay: %d %s", replay.Code, replay.Body.String())
	}
	denied := httptest.NewRecorder()
	control.Handler().ServeHTTP(denied, makeRequest(requestBody, false))
	if denied.Code != http.StatusNotFound {
		t.Fatalf("unauthenticated upload status: %d %s", denied.Code, denied.Body.String())
	}
	var localCount, stepCount, ciCount int
	if err := owner.QueryRow(ctx, "SELECT COUNT(*) FROM local_runs WHERE id=$1 AND principal_id=$2 AND source_verification='unverified'", receipt.LocalRunID, principal).Scan(&localCount); err != nil {
		t.Fatal(err)
	}
	if err := owner.QueryRow(ctx, "SELECT COUNT(*) FROM local_run_steps WHERE local_run_id=$1", receipt.LocalRunID).Scan(&stepCount); err != nil {
		t.Fatal(err)
	}
	if err := owner.QueryRow(ctx, "SELECT COUNT(*) FROM runs WHERE actor_id=$1", principal).Scan(&ciCount); err != nil {
		t.Fatal(err)
	}
	if localCount != 1 || stepCount != 1 || ciCount != 0 {
		t.Fatalf("offline ingestion history/dispatch isolation: local=%d step=%d ci=%d", localCount, stepCount, ciCount)
	}
}
