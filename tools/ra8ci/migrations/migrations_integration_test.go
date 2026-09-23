//go:build integration

package migrations

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"fmt"
	"os"
	"testing"
	"time"

	"github.com/jackc/pgx/v5/pgxpool"
)

func TestIntegrationFreshRunnerVMMigrations(t *testing.T) {
	dsn := os.Getenv("RA8CI_TEST_PG_DSN")
	if dsn == "" {
		t.Fatal("RA8CI_TEST_PG_DSN is required")
	}
	config, err := pgxpool.ParseConfig(dsn)
	if err != nil {
		t.Fatal(err)
	}
	if config.ConnConfig.Host != "127.0.0.1" || config.ConnConfig.Database != "ra8ci_test" {
		t.Fatalf("migration test requires disposable loopback ra8ci_test, got host=%q database=%q", config.ConnConfig.Host, config.ConnConfig.Database)
	}
	var suffix [8]byte
	if _, err := rand.Read(suffix[:]); err != nil {
		t.Fatal(err)
	}
	schema := "ra8ci_migrate_" + hex.EncodeToString(suffix[:])
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	admin, err := pgxpool.New(ctx, dsn)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(admin.Close)
	if _, err := admin.Exec(ctx, fmt.Sprintf(`CREATE SCHEMA %s`, schema)); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		cleanupCtx, cleanupCancel := context.WithTimeout(context.Background(), 10*time.Second)
		defer cleanupCancel()
		if _, err := admin.Exec(cleanupCtx, fmt.Sprintf(`DROP SCHEMA %s CASCADE`, schema)); err != nil {
			t.Errorf("remove isolated migration test schema: %v", err)
		}
	})
	config.ConnConfig.RuntimeParams["search_path"] = schema
	pool, err := pgxpool.NewWithConfig(ctx, config)
	if err != nil {
		t.Fatal(err)
	}
	defer pool.Close()
	if err := Apply(ctx, pool); err != nil {
		t.Fatalf("fresh schema migration: %v", err)
	}
	if err := Apply(ctx, pool); err != nil {
		t.Fatalf("repeat migration: %v", err)
	}
	var version, count int
	if err := pool.QueryRow(ctx, `SELECT max(version), count(*) FROM schema_migrations`).Scan(&version, &count); err != nil {
		t.Fatal(err)
	}
	if version != CurrentVersion() || count != CurrentVersion() {
		t.Fatalf("migration ledger: max=%d count=%d want=%d", version, count, CurrentVersion())
	}
	var columns int
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM information_schema.columns
		WHERE table_schema=$1 AND (table_name,column_name) IN
		(('runner_vms','cleanup_requested'),('runner_vms','template_digest'),
		 ('runner_vm_operations','expected_config_digest'),('runner_vm_operations','safety_runner_id'))`, schema).Scan(&columns); err != nil {
		t.Fatal(err)
	}
	if columns != 4 {
		t.Fatalf("fresh runner VM schema has %d of 4 safety columns", columns)
	}
}
