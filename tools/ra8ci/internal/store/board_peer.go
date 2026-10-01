package store

import (
	"crypto/tls"
	"crypto/x509"
	"time"
)

// checkedBoardPeer decides whether a TLS peer may be mapped to a board grant
// at all, before any of it is turned into an identity.
//
// The question is the one MTLSAuthorizer (internal/server/server.go) and
// verifiedAgentCertificate (internal/server/agent.go) already ask on the run
// and agent surfaces: is the leaf this peer presented the leaf the listener
// verified, and is that leaf usable right now. The board surface asked only
// the first half, and the second half is not implied by the handshake.
//
// The listener checks the validity window once, when the connection is made.
// Requests keep arriving on that connection afterwards: the HTTP server holds
// keep-alive connections open for its idle timeout, a board agent polls the
// same connection for as long as it is up, and a lease can outlive both. So a
// certificate that expires under a live connection keeps authorizing board
// commands (take, yield, free, checkpoint, neutral challenge) until the peer
// happens to reconnect, while the same certificate on the same process is
// refused the moment it asks for a run. A grant that has aged out should stop
// being a grant on every surface at the same instant, and an operator reading
// the audit trail should not have to know which door a request came through to
// know which rule it was held to.
//
// The window is the only thing added here. Whether the principal behind the
// certificate is still current, and whether it holds a grant on this board,
// stays the database's question and is answered by the query this guards:
// revoked_at and expires_at are re-read there against clock_timestamp().
func checkedBoardPeer(peer *tls.ConnectionState, repository, boardID string, now time.Time) error {
	if peer == nil || len(peer.PeerCertificates) == 0 || len(peer.VerifiedChains) == 0 ||
		len(peer.VerifiedChains[0]) == 0 || peer.PeerCertificates[0] == nil ||
		peer.VerifiedChains[0][0] == nil || len(peer.PeerCertificates[0].Raw) == 0 ||
		!peer.PeerCertificates[0].Equal(peer.VerifiedChains[0][0]) {
		return ErrDenied
	}
	if repository == "" || !validBoardID(boardID) {
		return ErrDenied
	}
	if !usableBoardLeaf(peer.PeerCertificates[0], now) {
		return ErrDenied
	}
	return nil
}

// usableBoardLeaf holds the verified leaf to its own validity window, with the
// same half-open reading the run and agent surfaces use: valid from NotBefore,
// and no longer valid at NotAfter itself.
func usableBoardLeaf(leaf *x509.Certificate, now time.Time) bool {
	return !now.Before(leaf.NotBefore) && now.Before(leaf.NotAfter)
}
