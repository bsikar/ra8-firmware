// Package migrations applies the forward-only PostgreSQL schema with a separate DDL role.
package migrations

import (
	"context"
	"embed"
	"errors"
	"fmt"
	"io/fs"
	"sort"
	"strconv"
	"strings"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgxpool"
)

//go:embed *.sql
var sqlFiles embed.FS

const currentVersion = 18

// CurrentVersion is the exact schema version accepted by the runtime store.
func CurrentVersion() int { return currentVersion }

// Apply runs pending, embedded migrations in one transaction. The caller must
// supply a migration-role pool; the runtime role must not have DDL privileges.
func Apply(ctx context.Context, pool *pgxpool.Pool) error {
	if pool == nil {
		return errors.New("migration pool is nil")
	}
	tx, err := pool.BeginTx(ctx, pgx.TxOptions{})
	if err != nil {
		return fmt.Errorf("begin migrations: %w", err)
	}
	defer func() { _ = tx.Rollback(ctx) }()
	if _, err = tx.Exec(ctx, "SELECT pg_advisory_xact_lock(72628801)"); err != nil {
		return fmt.Errorf("lock migrations: %w", err)
	}
	if _, err = tx.Exec(ctx, `CREATE TABLE IF NOT EXISTS schema_migrations (
		version integer PRIMARY KEY CHECK (version > 0),
		applied_at timestamptz NOT NULL DEFAULT clock_timestamp()
	)`); err != nil {
		return fmt.Errorf("bootstrap migration ledger: %w", err)
	}
	var version int
	if err = tx.QueryRow(ctx, "SELECT COALESCE(MAX(version), 0) FROM schema_migrations").Scan(&version); err != nil {
		return fmt.Errorf("read migration ledger: %w", err)
	}
	if version > currentVersion {
		return fmt.Errorf("database schema %d is newer than binary schema %d", version, currentVersion)
	}
	entries, err := fs.Glob(sqlFiles, "*.sql")
	if err != nil {
		return fmt.Errorf("list migrations: %w", err)
	}
	sort.Strings(entries)
	for _, name := range entries {
		parts := strings.SplitN(name, "_", 2)
		if len(parts) != 2 {
			return fmt.Errorf("invalid migration name %q", name)
		}
		v, parseErr := strconv.Atoi(parts[0])
		if parseErr != nil || v < 1 || v > currentVersion {
			return fmt.Errorf("invalid migration version in %q", name)
		}
		if v <= version {
			continue
		}
		if v != version+1 {
			return fmt.Errorf("migration gap before %q", name)
		}
		sql, readErr := sqlFiles.ReadFile(name)
		if readErr != nil {
			return fmt.Errorf("read migration %q: %w", name, readErr)
		}
		if _, err = tx.Exec(ctx, string(sql)); err != nil {
			return fmt.Errorf("apply migration %q: %w", name, err)
		}
		if _, err = tx.Exec(ctx, "INSERT INTO schema_migrations (version) VALUES ($1)", v); err != nil {
			return fmt.Errorf("record migration %q: %w", name, err)
		}
		version = v
	}
	if version != currentVersion {
		return fmt.Errorf("migration set incomplete: got %d, want %d", version, currentVersion)
	}
	if err = tx.Commit(ctx); err != nil {
		return fmt.Errorf("commit migrations: %w", err)
	}
	return nil
}
