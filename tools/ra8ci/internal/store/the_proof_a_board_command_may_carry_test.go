//go:build integration

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"context"
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/x509"
	"crypto/x509/pkix"
	"errors"
	"math/big"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
)

// The proof a board command may carry.
//
// Only a release and a completed recovery are answered with a neutral
// receipt. A proof attached to any other command is refused rather than
// ignored: a caller must never be able to spend a receipt on a command
// that was never going to consume it.
func TestIntegrationTheProofABoardCommandMayCarry(t *testing.T) {
	s, pool := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
	defer cancel()
	boardID := "board-command-" + mustID(t)
	agent := boardTestActor(t, ctx, s, pool, boardID, "agent", "submitter")
	now := time.Now().UTC()
	waiter := board.Waiter{ID: mustID(t), LeaseID: mustID(t), Holder: agent.ID(),
		Class: board.ClassAI, Reason: "a proof nobody asked for", Duration: time.Minute}

	t.Run("a proof offered with a command that consumes none", func(t *testing.T) {
		_, _, err := s.ApplyBoardCommand(ctx, agent, board.Enqueue{Waiter: waiter}, 0,
			&NeutralSubmission{ChallengeID: mustID(t), Receipt: []byte("receipt bytes")},
			exactNeutralVerifier{}, now)
		if !errors.Is(err, ErrDenied) {
			t.Fatalf("a receipt was spent on a queue request: %v", err)
		}
		// The denial is committed, so the board must be untouched behind it.
		var queued int
		if err := pool.QueryRow(ctx, "SELECT count(*) FROM board_waiters WHERE board_id=$1", boardID).
			Scan(&queued); err != nil {
			t.Fatal(err)
		}
		if queued != 0 {
			t.Fatalf("the refused command still queued %d waiters", queued)
		}
	})

	t.Run("the same command carrying no proof", func(t *testing.T) {
		after, _, err := s.ApplyBoardCommand(ctx, agent, board.Enqueue{Waiter: waiter}, 0, nil, nil, now)
		if err != nil {
			t.Fatalf("an ordinary queue request was refused: %v", err)
		}
		if after.Version == 0 {
			t.Fatalf("the accepted command did not advance the board: %+v", after)
		}
	})

	t.Run("a transition asked for with nothing to apply", func(t *testing.T) {
		if _, _, err := s.ApplyBoardCommand(ctx, agent, nil, 0, nil, nil, now); !errors.Is(err, ErrInvalid) {
			t.Fatalf("a nil command was applied: %v", err)
		}
		if _, _, err := s.ApplyBoardCommand(ctx, agent, board.Tick{}, 0, nil, nil, time.Time{}); !errors.Is(err, ErrInvalid) {
			t.Fatalf("a command was applied at no time at all: %v", err)
		}
	})
}

// The certificate the client surface will spend a grant for.
//
// The client door is the one an ordinary API caller reaches, and it is
// deliberately narrower than the board door: a principal's own kind has
// to pair with the grant role for the permission being asked for.
func TestIntegrationTheCertificateTheClientSurfaceAdmits(t *testing.T) {
	s, pool := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
	defer cancel()

	certFor := func(t *testing.T) []byte {
		t.Helper()
		key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
		if err != nil {
			t.Fatal(err)
		}
		template := &x509.Certificate{SerialNumber: big.NewInt(time.Now().UnixNano()),
			Subject: pkix.Name{CommonName: "client"}, NotBefore: time.Now().Add(-time.Minute),
			NotAfter: time.Now().Add(time.Hour)}
		der, err := x509.CreateCertificate(rand.Reader, template, template, &key.PublicKey, key)
		if err != nil {
			t.Fatal(err)
		}
		return der
	}

	t.Run("a scope request the plane will not read", func(t *testing.T) {
		for name, call := range map[string]func() (string, error){
			"no certificate": func() (string, error) {
				return s.AuthorizeCertificate(ctx, nil, boardTestRepo, PermissionSubmit)
			},
			"no repository": func() (string, error) {
				return s.AuthorizeCertificate(ctx, certFor(t), "", PermissionSubmit)
			},
			"a permission that does not exist": func() (string, error) {
				return s.AuthorizeCertificate(ctx, certFor(t), boardTestRepo, "runs:obliterate")
			},
		} {
			t.Run(name, func(t *testing.T) {
				if principal, err := call(); !errors.Is(err, ErrInvalid) || principal != "" {
					t.Fatalf("an unreadable scope request was answered: %q %v", principal, err)
				}
			})
		}
	})

	t.Run("a certificate no grant covers", func(t *testing.T) {
		if principal, err := s.AuthorizeCertificate(ctx, certFor(t), boardTestRepo, PermissionSubmit); !errors.Is(err, ErrDenied) ||
			principal != "" {
			t.Fatalf("an ungranted certificate was admitted: %q %v", principal, err)
		}
	})

	t.Run("a run nobody created", func(t *testing.T) {
		if repository, err := s.LookupRunRepository(ctx, mustID(t)); !errors.Is(err, ErrNotFound) || repository != "" {
			t.Fatalf("an unknown run had a repository: %q %v", repository, err)
		}
		if _, err := s.LookupRunRepository(ctx, "not-a-uuid"); !errors.Is(err, ErrInvalid) {
			t.Fatalf("an unusable run identifier was looked up: %v", err)
		}
	})

	t.Run("the denial the surface records before it answers", func(t *testing.T) {
		for name, call := range map[string]func() error{
			"no actor":  func() error { return s.AuditDenied(ctx, "", "runs.create", "target") },
			"no action": func() error { return s.AuditDenied(ctx, "someone", "", "target") },
			"no target": func() error { return s.AuditDenied(ctx, "someone", "runs.create", "") },
		} {
			t.Run(name, func(t *testing.T) {
				if err := call(); !errors.Is(err, ErrInvalid) {
					t.Fatalf("an incomplete denial was recorded: %v", err)
				}
			})
		}
		target := "run-" + mustID(t)
		if err := s.AuditDenied(ctx, "someone", "runs.create", target); err != nil {
			t.Fatalf("a complete denial was not recorded: %v", err)
		}
		var actor, outcome, targetType string
		if err := pool.QueryRow(ctx, `SELECT actor_id,outcome,target_type FROM audit
			WHERE action='runs.create' AND target_id=$1`, target).Scan(&actor, &outcome, &targetType); err != nil {
			t.Fatalf("the denial is not in the audit log: %v", err)
		}
		if actor != "someone" || outcome != "denied" || targetType != "api" {
			t.Fatalf("the denial recorded as actor=%s outcome=%s target_type=%s", actor, outcome, targetType)
		}
	})

	t.Run("the plane answering that it is ready", func(t *testing.T) {
		if err := s.Health(ctx); err != nil {
			t.Fatalf("a healthy plane reported otherwise: %v", err)
		}
		var readiness int
		if err := pool.QueryRow(ctx, `SELECT count(*) FROM audit
			WHERE action='server.readiness' AND target_id='control'`).Scan(&readiness); err != nil {
			t.Fatal(err)
		}
		if readiness == 0 {
			t.Fatal("a readiness check left no trace in the audit log")
		}
	})
}
