package store

import (
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"errors"
	"math/big"
	"testing"
	"time"
)

var boardPeerNow = time.Date(2026, 9, 26, 12, 0, 0, 0, time.UTC)

// boardPeerCert issues a leaf valid over the given window. The board surface
// never inspects anything but the window and the bytes, so nothing else about
// the certificate is arranged here.
func boardPeerCert(t *testing.T, notBefore, notAfter time.Time) *x509.Certificate {
	t.Helper()
	key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	template := &x509.Certificate{
		SerialNumber: big.NewInt(1),
		Subject:      pkix.Name{CommonName: "board-peer"},
		NotBefore:    notBefore,
		NotAfter:     notAfter,
	}
	der, err := x509.CreateCertificate(rand.Reader, template, template, &key.PublicKey, key)
	if err != nil {
		t.Fatal(err)
	}
	leaf, err := x509.ParseCertificate(der)
	if err != nil {
		t.Fatal(err)
	}
	return leaf
}

// boardPeerState is the shape a listener running RequireAndVerifyClientCert
// hands a handler: the presented leaf is also the verified one.
func boardPeerState(leaf *x509.Certificate) *tls.ConnectionState {
	return &tls.ConnectionState{
		PeerCertificates: []*x509.Certificate{leaf},
		VerifiedChains:   [][]*x509.Certificate{{leaf}},
	}
}

func currentBoardPeer(t *testing.T) *tls.ConnectionState {
	t.Helper()
	return boardPeerState(boardPeerCert(t, boardPeerNow.Add(-time.Hour), boardPeerNow.Add(time.Hour)))
}

func TestVerifiedBoardPeerInsideItsWindowIsAdmitted(t *testing.T) {
	if err := checkedBoardPeer(currentBoardPeer(t), "bsikar/ra8-firmware", "board-1", boardPeerNow); err != nil {
		t.Fatalf("a verified, unexpired peer must be admitted: %v", err)
	}
}

func TestExpiredBoardPeerIsDenied(t *testing.T) {
	peer := boardPeerState(boardPeerCert(t, boardPeerNow.Add(-2*time.Hour), boardPeerNow.Add(-time.Minute)))
	if err := checkedBoardPeer(peer, "bsikar/ra8-firmware", "board-1", boardPeerNow); !errors.Is(err, ErrDenied) {
		t.Fatalf("an expired leaf must be denied, got %v", err)
	}
}

// The listener checks the window at the handshake and never again. This is the
// case the board surface was missing: a connection made while the certificate
// was current, still carrying requests after it expired.
func TestBoardPeerThatExpiredUnderALiveConnectionIsDenied(t *testing.T) {
	leaf := boardPeerCert(t, boardPeerNow.Add(-time.Hour), boardPeerNow.Add(time.Minute))
	peer := boardPeerState(leaf)
	if err := checkedBoardPeer(peer, "bsikar/ra8-firmware", "board-1", boardPeerNow); err != nil {
		t.Fatalf("the handshake-time request must be admitted: %v", err)
	}
	later := boardPeerNow.Add(10 * time.Minute)
	if err := checkedBoardPeer(peer, "bsikar/ra8-firmware", "board-1", later); !errors.Is(err, ErrDenied) {
		t.Fatalf("a later request on the same connection must be denied, got %v", err)
	}
}

func TestBoardPeerNotYetValidIsDenied(t *testing.T) {
	peer := boardPeerState(boardPeerCert(t, boardPeerNow.Add(time.Minute), boardPeerNow.Add(time.Hour)))
	if err := checkedBoardPeer(peer, "bsikar/ra8-firmware", "board-1", boardPeerNow); !errors.Is(err, ErrDenied) {
		t.Fatalf("a leaf that is not valid yet must be denied, got %v", err)
	}
}

// Half-open, the same reading MTLSAuthorizer and verifiedAgentCertificate use:
// valid at NotBefore, no longer valid at NotAfter itself.
func TestBoardPeerWindowIsHalfOpen(t *testing.T) {
	start := boardPeerNow
	end := boardPeerNow.Add(time.Hour)
	peer := boardPeerState(boardPeerCert(t, start, end))
	if err := checkedBoardPeer(peer, "bsikar/ra8-firmware", "board-1", start); err != nil {
		t.Fatalf("NotBefore itself must be admitted: %v", err)
	}
	if err := checkedBoardPeer(peer, "bsikar/ra8-firmware", "board-1", end); !errors.Is(err, ErrDenied) {
		t.Fatalf("NotAfter itself must be denied, got %v", err)
	}
}

func TestUnverifiedBoardPeerIsDenied(t *testing.T) {
	leaf := boardPeerCert(t, boardPeerNow.Add(-time.Hour), boardPeerNow.Add(time.Hour))
	for name, peer := range map[string]*tls.ConnectionState{
		"nil state":         nil,
		"nothing presented": {VerifiedChains: [][]*x509.Certificate{{leaf}}},
		"nothing verified":  {PeerCertificates: []*x509.Certificate{leaf}},
		"empty chain": {PeerCertificates: []*x509.Certificate{leaf},
			VerifiedChains: [][]*x509.Certificate{{}}},
		"nil chain entry": {PeerCertificates: []*x509.Certificate{leaf},
			VerifiedChains: [][]*x509.Certificate{{nil}}},
		"a different leaf than the one verified": {
			PeerCertificates: []*x509.Certificate{boardPeerCert(t, boardPeerNow.Add(-time.Hour), boardPeerNow.Add(time.Hour))},
			VerifiedChains:   [][]*x509.Certificate{{leaf}}},
	} {
		if err := checkedBoardPeer(peer, "bsikar/ra8-firmware", "board-1", boardPeerNow); !errors.Is(err, ErrDenied) {
			t.Fatalf("%s must be denied, got %v", name, err)
		}
	}
}

func TestBoardPeerStillRequiresARepositoryAndBoard(t *testing.T) {
	peer := currentBoardPeer(t)
	for name, tc := range map[string]struct{ repository, boardID string }{
		"no repository": {repository: "", boardID: "board-1"},
		"no board":      {repository: "bsikar/ra8-firmware", boardID: ""},
		"padded board":  {repository: "bsikar/ra8-firmware", boardID: " board-1"},
		"oversized board": {repository: "bsikar/ra8-firmware",
			boardID: string(make([]byte, 129))},
	} {
		if err := checkedBoardPeer(peer, tc.repository, tc.boardID, boardPeerNow); !errors.Is(err, ErrDenied) {
			t.Fatalf("%s must be denied, got %v", name, err)
		}
	}
}

// The window is the only rule added. Nothing here decides whether the
// principal is current or holds a grant: that stays the query's question.
func TestBoardPeerSaysNothingAboutGrants(t *testing.T) {
	if err := checkedBoardPeer(currentBoardPeer(t), "some/other-repo", "board-9", boardPeerNow); err != nil {
		t.Fatalf("an unknown repository and board are the query's refusal, not this one: %v", err)
	}
}

func TestBoardLeafWindowMatchesTheRunSurface(t *testing.T) {
	leaf := boardPeerCert(t, boardPeerNow, boardPeerNow.Add(time.Hour))
	for _, tc := range []struct {
		name string
		now  time.Time
		want bool
	}{
		{name: "before the window", now: boardPeerNow.Add(-time.Nanosecond), want: false},
		{name: "at NotBefore", now: boardPeerNow, want: true},
		{name: "inside", now: boardPeerNow.Add(time.Minute), want: true},
		{name: "at NotAfter", now: boardPeerNow.Add(time.Hour), want: false},
		{name: "after", now: boardPeerNow.Add(2 * time.Hour), want: false},
	} {
		if got := usableBoardLeaf(leaf, tc.now); got != tc.want {
			t.Fatalf("%s: usableBoardLeaf = %v, want %v", tc.name, got, tc.want)
		}
	}
}
