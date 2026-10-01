//go:build integration

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"context"
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/sha256"
	"crypto/tls"
	"crypto/x509"
	"encoding/hex"
	"errors"
	"math/big"
	"strings"
	"testing"
	"time"

	"github.com/jackc/pgx/v5/pgxpool"
)

// plantedPeer registers a principal and one grant straight into the
// database and hands back the connection state a listener would give a
// handler for it. Unlike boardTestActor it never authorizes, so the door
// itself is what is under test.
func plantedPeer(t *testing.T, ctx context.Context, pool *pgxpool.Pool, kind, role, repository, grantBoard string,
	revoked bool, lifetime time.Duration) *tls.ConnectionState {
	t.Helper()
	key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	template := &x509.Certificate{SerialNumber: big.NewInt(time.Now().UnixNano()),
		NotBefore: time.Now().Add(-time.Minute), NotAfter: time.Now().Add(time.Hour),
		KeyUsage: x509.KeyUsageDigitalSignature, ExtKeyUsage: []x509.ExtKeyUsage{x509.ExtKeyUsageClientAuth}}
	der, err := x509.CreateCertificate(rand.Reader, template, template, &key.PublicKey, key)
	if err != nil {
		t.Fatal(err)
	}
	cert, err := x509.ParseCertificate(der)
	if err != nil {
		t.Fatal(err)
	}
	sum := sha256.Sum256(cert.Raw)
	id := mustID(t)
	revokedAt := "NULL"
	if revoked {
		revokedAt = "clock_timestamp()"
	}
	if _, err := pool.Exec(ctx, `INSERT INTO api_principals
		(cert_sha256,principal_id,kind,expires_at,revoked_at)
		VALUES ($1,$2,$3,clock_timestamp()+$4::interval,`+revokedAt+`)`,
		hex.EncodeToString(sum[:]), id, kind, lifetime.String()); err != nil {
		t.Fatal(err)
	}
	if _, err := pool.Exec(ctx, `INSERT INTO api_grants (principal_id,repository,role,board_id)
		VALUES ($1,$2,$3,$4)`, id, repository, role, grantBoard); err != nil {
		t.Fatal(err)
	}
	return &tls.ConnectionState{PeerCertificates: []*x509.Certificate{cert},
		VerifiedChains: [][]*x509.Certificate{{cert}}}
}

// The peer a board door admits.
//
// Every board call starts here: a verified certificate is turned into an
// actor only when a live principal holds a grant that names this
// repository, this board, and a role its own kind is allowed to hold.
// Each half of that is its own way in, so each is refused on its own.
func TestIntegrationThePeerABoardDoorAdmits(t *testing.T) {
	s, pool := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
	defer cancel()
	boardID := "board-door-" + mustID(t)
	elsewhere := "board-door-" + mustID(t)
	hour := time.Hour

	refused := map[string]*tls.ConnectionState{
		"a certificate nobody registered": {
			PeerCertificates: []*x509.Certificate{boardPeerCert(t, time.Now().Add(-time.Minute), time.Now().Add(time.Hour))},
			VerifiedChains: [][]*x509.Certificate{{boardPeerCert(t,
				time.Now().Add(-time.Minute), time.Now().Add(time.Hour))}},
		},
		"a grant for another repository": plantedPeer(t, ctx, pool, "board_agent", "board_agent",
			"bsikar/somewhere-else", boardID, false, hour),
		"a grant for another board": plantedPeer(t, ctx, pool, "board_agent", "board_agent",
			boardTestRepo, elsewhere, false, hour),
		"a principal that has been revoked": plantedPeer(t, ctx, pool, "board_agent", "board_agent",
			boardTestRepo, boardID, true, hour),
		"a principal whose registration has expired": plantedPeer(t, ctx, pool, "board_agent", "board_agent",
			boardTestRepo, boardID, false, -time.Minute),
		"a role this kind of principal cannot hold": plantedPeer(t, ctx, pool, "human", "board_agent",
			boardTestRepo, boardID, false, hour),
		"a submitter grant held by a board agent": plantedPeer(t, ctx, pool, "board_agent", "submitter",
			boardTestRepo, boardID, false, hour),
	}
	for name, peer := range refused {
		t.Run(name, func(t *testing.T) {
			actor, err := s.AuthorizeBoardPeer(ctx, peer, boardTestRepo, boardID)
			if !errors.Is(err, ErrDenied) || actor.ID() != "" {
				t.Fatalf("the door admitted it: actor=%+v err=%v", actor, err)
			}
		})
	}

	t.Run("an operator holding no board at all", func(t *testing.T) {
		// The one grant that is deliberately board-less: an operator is
		// admitted on any board, which is how recovery stays possible on
		// a board nobody has been granted.
		peer := plantedPeer(t, ctx, pool, "human", "operator", boardTestRepo, "", false, hour)
		actor, err := s.AuthorizeBoardPeer(ctx, peer, boardTestRepo, boardID)
		if err != nil || actor.role != "operator" || actor.kind != "human" {
			t.Fatalf("the operator was not admitted: actor=%+v err=%v", actor, err)
		}
		if actor.boardID != boardID || actor.repository != boardTestRepo || len(actor.certificate) != 64 {
			t.Fatalf("the admitted actor was not bound to the door it came through: %+v", actor)
		}
	})

	t.Run("the board agent the door is for", func(t *testing.T) {
		peer := plantedPeer(t, ctx, pool, "board_agent", "board_agent", boardTestRepo, boardID, false, hour)
		actor, err := s.AuthorizeBoardPeer(ctx, peer, boardTestRepo, boardID)
		if err != nil || actor.kind != "board_agent" || actor.role != "board_agent" ||
			actor.boardID != boardID || actor.ID() == "" {
			t.Fatalf("the board agent was not admitted: actor=%+v err=%v", actor, err)
		}
		sum := sha256.Sum256(peer.PeerCertificates[0].Raw)
		if actor.certificate != hex.EncodeToString(sum[:]) {
			t.Fatalf("the actor carries a certificate that is not the one presented: %s", actor.certificate)
		}
	})
}

// The fixture profile a board is pinned to.
//
// Neutral receipts and HIL history are bound to one operator-approved
// fixture, so a board with no approved profile has to read as absent
// rather than as an empty profile anyone could match.
func TestIntegrationTheFixtureProfileABoardIsPinnedTo(t *testing.T) {
	s, pool := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
	defer cancel()
	boardID := "board-fixture-" + mustID(t)

	t.Run("a board with no approved profile", func(t *testing.T) {
		profile, err := s.ApprovedBoardFixtureProfile(ctx, boardID)
		if !errors.Is(err, ErrNotFound) || profile != (BoardFixtureProfile{}) {
			t.Fatalf("an unapproved board read as %+v: %v", profile, err)
		}
	})

	t.Run("a board identifier the plane will not read", func(t *testing.T) {
		if _, err := s.ApprovedBoardFixtureProfile(ctx, ""); !errors.Is(err, ErrInvalid) {
			t.Fatalf("an empty board identifier was read: %v", err)
		}
	})

	t.Run("the approved profile itself", func(t *testing.T) {
		sum := strings.Repeat("b", 64)
		if _, err := pool.Exec(ctx, `INSERT INTO board_fixture_profiles
			(board_id,fixture_revision,profile_sha256,restore_policy)
			VALUES ($1,'fixture-v2',$2,'restore-image')`, boardID, sum); err != nil {
			t.Fatal(err)
		}
		profile, err := s.ApprovedBoardFixtureProfile(ctx, boardID)
		if err != nil {
			t.Fatalf("the approved profile was not readable: %v", err)
		}
		if profile.BoardID != boardID || profile.FixtureRevision != "fixture-v2" ||
			profile.ProfileSHA256 != sum || profile.RestorePolicy != "restore-image" {
			t.Fatalf("the approved profile read back as %+v", profile)
		}
	})
}
