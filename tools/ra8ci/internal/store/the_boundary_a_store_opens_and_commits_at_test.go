//go:build integration

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"context"
	"errors"
	"strings"
	"testing"
	"time"

	"github.com/jackc/pgx/v5"
)

// The DSN a store will open.
//
// An empty DSN is a configuration mistake and is answered as one, before a
// connection is attempted. A DSN the driver cannot read is an unavailable
// database, never a nil store handed back with a nil error.
func TestIntegrationTheDSNAStoreWillOpen(t *testing.T) {
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()

	for _, dsn := range []string{"", "   ", "\t\n"} {
		s, err := Open(ctx, dsn)
		if !errors.Is(err, ErrInvalid) || s != nil || !strings.Contains(err.Error(), "database DSN is empty") {
			t.Fatalf("Open(%q) answered %v %v", dsn, s, err)
		}
	}

	s, err := Open(ctx, "postgres://ra8ci@127.0.0.1:not-a-port/ra8ci_test")
	if !errors.Is(err, ErrUnavailable) || s != nil || !strings.Contains(err.Error(), "connect") {
		t.Fatalf("an unreadable DSN opened: %v %v", s, err)
	}
}

// The boundary a store commits at.
//
// withTx commits only when the work inside it succeeds. When the work
// fails, its error reaches the caller exactly as raised, not rewrapped as
// an unavailable database, and nothing the work wrote survives.
func TestIntegrationTheBoundaryAStoreCommitsAt(t *testing.T) {
	s, pool := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	written := func(t *testing.T, target string) int {
		t.Helper()
		var n int
		if err := pool.QueryRow(ctx, `SELECT count(*) FROM audit
			WHERE action='store.boundary' AND target_type='test' AND target_id=$1`, target).Scan(&n); err != nil {
			t.Fatal(err)
		}
		return n
	}
	write := func(tx pgx.Tx, target string) error {
		return appendAudit(ctx, tx, "ra8ci-server", "store.boundary", "test", target, "ok", "", "", "", nil)
	}

	t.Run("work that fails", func(t *testing.T) {
		target := "boundary-" + mustID(t)
		refusal := errors.New("the work refused itself")
		err := s.withTx(ctx, "boundary", func(tx pgx.Tx) error {
			if err := write(tx, target); err != nil {
				t.Fatal(err)
			}
			return refusal
		})
		if !errors.Is(err, refusal) || errors.Is(err, ErrUnavailable) {
			t.Fatalf("the work's own error did not reach the caller as raised: %v", err)
		}
		if n := written(t, target); n != 0 {
			t.Fatalf("failed work left %d rows behind", n)
		}
	})

	t.Run("work that succeeds", func(t *testing.T) {
		target := "boundary-" + mustID(t)
		if err := s.withTx(ctx, "boundary", func(tx pgx.Tx) error { return write(tx, target) }); err != nil {
			t.Fatalf("successful work was not committed: %v", err)
		}
		if n := written(t, target); n != 1 {
			t.Fatalf("successful work committed %d rows", n)
		}
	})
}
