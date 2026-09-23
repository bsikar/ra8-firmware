// Package boardclient provides a fail-closed mTLS client for the durable board
// lease service. It never touches hardware or implements a local lockfile.
package boardclient

import (
	"bytes"
	"context"
	"crypto/tls"
	"crypto/x509"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"os"
	"strings"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

const maxResponseBytes = 1 << 20

var (
	ErrInvalidConfig       = errors.New("invalid board client configuration")
	ErrInvalidRequest      = errors.New("invalid board request")
	ErrNotQueued           = errors.New("board request is no longer queued")
	ErrAlreadyGranted      = errors.New("board waiter has already been granted")
	ErrStaleLease          = errors.New("board lease is absent or generation changed")
	ErrYieldRequested      = errors.New("board lease must yield before any new work")
	ErrNoYieldRequest      = errors.New("board has not requested a yield")
	ErrRecoveryRequired    = errors.New("board requires recovery or is quarantined")
	ErrNeutralUnavailable  = errors.New("authenticated board-agent neutral receipt producer is unavailable")
	ErrInvalidNeutralProof = errors.New("board-agent neutral receipt is absent or invalid")
)

type Config struct {
	ServerURL    string
	CAFile       string
	CertFile     string
	KeyFile      string
	PollInterval time.Duration
}

// Client uses a verified server certificate and a client certificate. The
// server additionally checks database grants; a TLS connection is not a lease.
type Client struct {
	base *url.URL
	http *http.Client
	poll time.Duration
}

func New(config Config) (*Client, error) {
	base, err := url.Parse(config.ServerURL)
	if err != nil || base.Scheme != "https" || base.Host == "" || base.User != nil ||
		(base.Path != "" && base.Path != "/") || base.RawQuery != "" || base.Fragment != "" ||
		config.CAFile == "" || config.CertFile == "" || config.KeyFile == "" || config.PollInterval < 0 {
		return nil, ErrInvalidConfig
	}
	caPEM, err := os.ReadFile(config.CAFile)
	if err != nil {
		return nil, fmt.Errorf("%w: read server CA: %v", ErrInvalidConfig, err)
	}
	roots := x509.NewCertPool()
	if !roots.AppendCertsFromPEM(caPEM) {
		return nil, fmt.Errorf("%w: server CA has no certificates", ErrInvalidConfig)
	}
	certificate, err := tls.LoadX509KeyPair(config.CertFile, config.KeyFile)
	if err != nil {
		return nil, fmt.Errorf("%w: client certificate: %v", ErrInvalidConfig, err)
	}
	poll := config.PollInterval
	if poll == 0 {
		poll = time.Second
	}
	base.Path, base.RawPath = "", ""
	transport := &http.Transport{TLSClientConfig: &tls.Config{
		MinVersion: tls.VersionTLS13, RootCAs: roots, Certificates: []tls.Certificate{certificate},
	}}
	return &Client{base: base, poll: poll, http: &http.Client{
		Transport:     transport,
		CheckRedirect: func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse },
	}}, nil
}

func (c *Client) CloseIdleConnections() {
	if c != nil && c.http != nil {
		c.http.CloseIdleConnections()
	}
}

type HTTPError struct {
	Status    int
	Code      string
	Detail    string
	Retryable bool
}

func (e *HTTPError) Error() string {
	return fmt.Sprintf("board API status %d (%s): %s", e.Status, e.Code, e.Detail)
}

func (e *HTTPError) Is(target error) bool {
	other, ok := target.(*HTTPError)
	return ok && e.Status == other.Status
}

var ErrNotFound = &HTTPError{Status: http.StatusNotFound}

type Ticket struct {
	BoardID   string
	RequestID string
	LeaseID   string
	Class     board.Class
	Why       string
	Duration  time.Duration
}

type LeaseToken struct {
	BoardID    string
	RequestID  string
	LeaseID    string
	Generation uint64
	ExpiresAt  time.Time
	Version    uint64
}

// NeutralReceiptProducer must obtain a signed, challenge-bound receipt from
// the authenticated board agent after it actually neutralizes the fixture.
// The server verifies the signature and one-use challenge inside its CAS tx.
// Passing nil never causes a forged or assumed-neutral release.
type NeutralReceiptProducer interface {
	ProduceNeutralReceipt(context.Context, store.NeutralChallenge) ([]byte, error)
}

func validBoardID(id string) bool {
	if id == "" || len(id) > 128 {
		return false
	}
	for _, r := range id {
		if !((r >= 'a' && r <= 'z') || (r >= 'A' && r <= 'Z') || (r >= '0' && r <= '9') || r == '-' || r == '_' || r == '.') {
			return false
		}
	}
	return true
}

func boardPath(boardID, suffix string) string {
	return "/v1/boards/" + url.PathEscape(boardID) + suffix
}

func (c *Client) request(ctx context.Context, method, path string, input, output any) error {
	if c == nil || c.http == nil || c.base == nil {
		return ErrInvalidConfig
	}
	var body io.Reader
	if input != nil {
		encoded, err := json.Marshal(input)
		if err != nil {
			return fmt.Errorf("%w: encode request: %v", ErrInvalidRequest, err)
		}
		body = bytes.NewReader(encoded)
	}
	endpoint := *c.base
	endpoint.Path = path
	req, err := http.NewRequestWithContext(ctx, method, endpoint.String(), body)
	if err != nil {
		return fmt.Errorf("%w: HTTP request: %v", ErrInvalidRequest, err)
	}
	if input != nil {
		req.Header.Set("Content-Type", "application/json")
	}
	resp, err := c.http.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	limited := io.LimitReader(resp.Body, maxResponseBytes+1)
	raw, err := io.ReadAll(limited)
	if err != nil {
		return err
	}
	if len(raw) > maxResponseBytes {
		return fmt.Errorf("%w: server response exceeds limit", ErrInvalidRequest)
	}
	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		problem := struct {
			Code      string `json:"code"`
			Detail    string `json:"detail"`
			Retryable bool   `json:"retryable"`
		}{}
		_ = json.Unmarshal(raw, &problem)
		return &HTTPError{Status: resp.StatusCode, Code: problem.Code,
			Detail: problem.Detail, Retryable: problem.Retryable}
	}
	if output != nil {
		decoder := json.NewDecoder(bytes.NewReader(raw))
		decoder.DisallowUnknownFields()
		if err := decoder.Decode(output); err != nil {
			return fmt.Errorf("%w: malformed server response: %v", ErrInvalidRequest, err)
		}
		var trailing any
		if err := decoder.Decode(&trailing); !errors.Is(err, io.EOF) {
			return fmt.Errorf("%w: trailing server response", ErrInvalidRequest)
		}
	}
	return nil
}

func (c *Client) Status(ctx context.Context, boardID string) (board.Snapshot, error) {
	if !validBoardID(boardID) {
		return board.Snapshot{}, ErrInvalidRequest
	}
	var snapshot board.Snapshot
	if err := c.request(ctx, http.MethodGet, boardPath(boardID, ""), nil, &snapshot); err != nil {
		return board.Snapshot{}, err
	}
	if snapshot.BoardID != boardID || board.Validate(snapshot) != nil {
		return board.Snapshot{}, fmt.Errorf("%w: invalid board snapshot", ErrInvalidRequest)
	}
	return snapshot, nil
}

type commandResponse struct {
	Snapshot board.Snapshot `json:"snapshot"`
	Events   []board.Event  `json:"events"`
}

func (c *Client) command(ctx context.Context, boardID, suffix string, request any) (board.Snapshot, error) {
	var response commandResponse
	if err := c.request(ctx, http.MethodPost, boardPath(boardID, suffix), request, &response); err != nil {
		return board.Snapshot{}, err
	}
	if response.Snapshot.BoardID != boardID || board.Validate(response.Snapshot) != nil {
		return board.Snapshot{}, fmt.Errorf("%w: invalid command result", ErrInvalidRequest)
	}
	return response.Snapshot, nil
}

func isConflict(err error) bool {
	var response *HTTPError
	return errors.As(err, &response) && response.Status == http.StatusConflict
}

func snapshotOrEmpty(ctx context.Context, c *Client, boardID string) (board.Snapshot, error) {
	snapshot, err := c.Status(ctx, boardID)
	if errors.Is(err, ErrNotFound) {
		return board.New(boardID)
	}
	return snapshot, err
}

func visibleTicket(snapshot board.Snapshot, ticket Ticket) bool {
	if snapshot.Lease != nil && snapshot.Lease.ID == ticket.LeaseID && snapshot.Lease.WaiterID == ticket.RequestID {
		return true
	}
	for _, waiter := range snapshot.Queue {
		if waiter.ID == ticket.RequestID && waiter.LeaseID == ticket.LeaseID {
			return true
		}
	}
	return false
}

func takeClassName(class board.Class) string {
	switch class {
	case board.ClassHuman:
		return "human"
	case board.ClassCI:
		return "ci"
	case board.ClassAI:
		return "agent"
	default:
		return ""
	}
}

func classDurationLimit(class board.Class) time.Duration {
	switch class {
	case board.ClassHuman:
		return 8 * time.Hour
	case board.ClassCI:
		return 2 * time.Hour
	case board.ClassAI:
		return time.Hour
	default:
		return 0
	}
}

// RequestTake submits one durable waiter. Ticket IDs are returned even when
// transport fails so a caller can reconcile or cancel an ambiguous submission.
// The server, not this client, derives holder identity from the certificate
// and validates the requested priority against its database grant.
func (c *Client) RequestTake(ctx context.Context, boardID string, class board.Class, why string, duration time.Duration) (Ticket, error) {
	if !validBoardID(boardID) || takeClassName(class) == "" || why == "" ||
		len(why) > 500 || strings.TrimSpace(why) != why || duration <= 0 ||
		duration%time.Second != 0 || duration > classDurationLimit(class) {
		return Ticket{}, ErrInvalidRequest
	}
	requestID, err := store.NewID()
	if err != nil {
		return Ticket{}, err
	}
	leaseID, err := store.NewID()
	if err != nil {
		return Ticket{}, err
	}
	ticket := Ticket{BoardID: boardID, RequestID: requestID, LeaseID: leaseID,
		Class: class, Why: why, Duration: duration}
	for {
		snapshot, err := snapshotOrEmpty(ctx, c, boardID)
		if err != nil {
			return ticket, err
		}
		if visibleTicket(snapshot, ticket) {
			return ticket, nil
		}
		result, err := c.command(ctx, boardID, "/take", struct {
			ExpectedVersion uint64 `json:"expected_version"`
			RequestID       string `json:"request_id"`
			LeaseID         string `json:"lease_id"`
			Class           string `json:"class"`
			Why             string `json:"why"`
			DurationSeconds int64  `json:"duration_seconds"`
		}{snapshot.Version, ticket.RequestID, ticket.LeaseID, takeClassName(class), why, int64(duration / time.Second)})
		if err == nil && !visibleTicket(result, ticket) {
			return ticket, fmt.Errorf("%w: accepted take is absent from board snapshot", ErrInvalidRequest)
		}
		if err == nil {
			return ticket, nil
		}
		if !isConflict(err) {
			return ticket, err
		}
		if err := waitConflict(ctx); err != nil {
			return ticket, err
		}
	}
}

// WaitForGrant blocks until the board agent acknowledges this exact grant.
// A pending grant or queued waiter is not authority to touch the board.
func (c *Client) WaitForGrant(ctx context.Context, ticket Ticket) (LeaseToken, error) {
	if !validBoardID(ticket.BoardID) || !store.ValidID(ticket.RequestID) || !store.ValidID(ticket.LeaseID) {
		return LeaseToken{}, ErrInvalidRequest
	}
	for {
		snapshot, err := c.Status(ctx, ticket.BoardID)
		if err != nil {
			return LeaseToken{}, err
		}
		if snapshot.Phase == board.Quarantined || snapshot.Phase == board.Recovering || snapshot.Phase == board.RecoveryRequired {
			return LeaseToken{}, ErrRecoveryRequired
		}
		if snapshot.Lease != nil && snapshot.Lease.ID == ticket.LeaseID && snapshot.Lease.WaiterID == ticket.RequestID {
			if !time.Now().Before(snapshot.Lease.ExpiresAt) {
				return LeaseToken{}, ErrRecoveryRequired
			}
			token := LeaseToken{BoardID: ticket.BoardID, RequestID: ticket.RequestID,
				LeaseID: ticket.LeaseID, Generation: snapshot.Lease.Generation,
				ExpiresAt: snapshot.Lease.ExpiresAt, Version: snapshot.Version}
			switch snapshot.Phase {
			case board.Active:
				return token, nil
			case board.YieldRequested, board.Draining:
				return token, ErrYieldRequested
			}
		} else if !visibleTicket(snapshot, ticket) {
			return LeaseToken{}, ErrNotQueued
		}
		if err := waitPoll(ctx, c.poll); err != nil {
			return LeaseToken{}, err
		}
	}
}

// WaitForYieldRequest polls the durable lease until a higher-priority waiter
// asks the holder to yield. It returns only a server-validated snapshot; the
// caller must finish its current indivisible segment before Checkpoint.
func (c *Client) WaitForYieldRequest(ctx context.Context, token LeaseToken) (board.Snapshot, error) {
	if c == nil || ctx == nil || token.ExpiresAt.IsZero() {
		return board.Snapshot{}, ErrInvalidRequest
	}
	for {
		snapshot, err := c.leaseStatus(ctx, token)
		if err != nil {
			return board.Snapshot{}, err
		}
		switch snapshot.Phase {
		case board.YieldRequested, board.Draining:
			return snapshot, nil
		case board.Active:
			// Continue polling while this exact generation still owns the board.
		case board.Quarantined, board.Recovering, board.RecoveryRequired:
			return board.Snapshot{}, ErrRecoveryRequired
		default:
			return board.Snapshot{}, ErrStaleLease
		}
		if err := waitPoll(ctx, c.poll); err != nil {
			return board.Snapshot{}, err
		}
	}
}

func waitPoll(ctx context.Context, duration time.Duration) error {
	if duration <= 0 {
		duration = time.Second
	}
	timer := time.NewTimer(duration)
	defer timer.Stop()
	select {
	case <-ctx.Done():
		return ctx.Err()
	case <-timer.C:
		return nil
	}
}

func waitConflict(ctx context.Context) error {
	return waitPoll(ctx, 50*time.Millisecond)
}

// Cancel removes a still-queued waiter using current snapshot versions. It
// cannot cancel a granted lease; that lease must be neutralized and released.
func (c *Client) Cancel(ctx context.Context, ticket Ticket) error {
	if !validBoardID(ticket.BoardID) || !store.ValidID(ticket.RequestID) || !store.ValidID(ticket.LeaseID) {
		return ErrInvalidRequest
	}
	for {
		snapshot, err := c.Status(ctx, ticket.BoardID)
		if err != nil {
			return err
		}
		if snapshot.Lease != nil && snapshot.Lease.ID == ticket.LeaseID && snapshot.Lease.WaiterID == ticket.RequestID {
			return ErrAlreadyGranted
		}
		if !visibleTicket(snapshot, ticket) {
			return nil
		}
		_, err = c.command(ctx, ticket.BoardID, "/waiters/"+url.PathEscape(ticket.RequestID)+"/cancel",
			struct {
				ExpectedVersion uint64 `json:"expected_version"`
			}{snapshot.Version})
		if err == nil {
			return nil
		}
		if !isConflict(err) {
			return err
		}
		if err := waitConflict(ctx); err != nil {
			return err
		}
	}
}

func sameLease(snapshot board.Snapshot, token LeaseToken) bool {
	return snapshot.Lease != nil && snapshot.Lease.ID == token.LeaseID &&
		snapshot.Lease.WaiterID == token.RequestID && snapshot.Lease.Generation == token.Generation
}

func (c *Client) leaseStatus(ctx context.Context, token LeaseToken) (board.Snapshot, error) {
	if !validBoardID(token.BoardID) || !store.ValidID(token.LeaseID) ||
		!store.ValidID(token.RequestID) || token.Generation == 0 {
		return board.Snapshot{}, ErrInvalidRequest
	}
	snapshot, err := c.Status(ctx, token.BoardID)
	if err != nil {
		return board.Snapshot{}, err
	}
	if !sameLease(snapshot, token) {
		return board.Snapshot{}, ErrStaleLease
	}
	return snapshot, nil
}

// CanStartSegment is only a server-state check. The board agent must also
// check its durable generation and local monotonic deadline under its hardware
// mutex immediately before touching the fixture.
func (c *Client) CanStartSegment(ctx context.Context, token LeaseToken, bound, recoveryMargin time.Duration) error {
	snapshot, err := c.leaseStatus(ctx, token)
	if err != nil {
		return err
	}
	return board.CanStartSegment(snapshot, board.Token{BoardID: token.BoardID,
		LeaseID: token.LeaseID, Generation: token.Generation}, time.Now().UTC(), bound, recoveryMargin)
}

// Checkpoint cooperatively begins draining only after a yield request. It
// does not free the board; Free still requires a verified neutral receipt.
func (c *Client) Checkpoint(ctx context.Context, token LeaseToken) (board.Snapshot, error) {
	for {
		snapshot, err := c.leaseStatus(ctx, token)
		if err != nil {
			return board.Snapshot{}, err
		}
		if snapshot.Phase == board.Draining {
			return snapshot, nil
		}
		if snapshot.Phase != board.YieldRequested {
			return board.Snapshot{}, ErrNoYieldRequest
		}
		result, err := c.command(ctx, token.BoardID, "/checkpoint", struct {
			ExpectedVersion uint64 `json:"expected_version"`
			LeaseID         string `json:"lease_id"`
			Generation      uint64 `json:"generation"`
		}{snapshot.Version, token.LeaseID, token.Generation})
		if err == nil || !isConflict(err) {
			return result, err
		}
		if err := waitConflict(ctx); err != nil {
			return board.Snapshot{}, err
		}
	}
}

// Extend requests a later UTC expiry; class ceilings and contended extension
// limits remain server-side and cannot be bypassed by changing this client.
func (c *Client) Extend(ctx context.Context, token LeaseToken, expiry time.Time, why string) (board.Snapshot, error) {
	if expiry.IsZero() || why == "" || len(why) > 500 || strings.TrimSpace(why) != why {
		return board.Snapshot{}, ErrInvalidRequest
	}
	for {
		snapshot, err := c.leaseStatus(ctx, token)
		if err != nil {
			return board.Snapshot{}, err
		}
		if snapshot.Phase != board.Active && snapshot.Phase != board.YieldRequested && snapshot.Phase != board.Draining {
			return board.Snapshot{}, ErrStaleLease
		}
		result, err := c.command(ctx, token.BoardID, "/leases/"+url.PathEscape(token.LeaseID)+"/extend", struct {
			ExpectedVersion uint64    `json:"expected_version"`
			Generation      uint64    `json:"generation"`
			NewExpiry       time.Time `json:"new_expiry"`
			Why             string    `json:"why"`
		}{snapshot.Version, token.Generation, expiry.UTC(), why})
		if err == nil || !isConflict(err) {
			return result, err
		}
		if err := waitConflict(ctx); err != nil {
			return board.Snapshot{}, err
		}
	}
}

// Free never assumes that a stopped task left safe hardware. It requests a
// one-use challenge, asks the board agent to neutralize and sign that exact
// challenge, then sends the opaque proof to the server for transactional
// verification. With no producer, no release request is sent.
func (c *Client) Free(ctx context.Context, token LeaseToken, producer NeutralReceiptProducer) (board.Snapshot, error) {
	if producer == nil {
		return board.Snapshot{}, ErrNeutralUnavailable
	}
	for {
		snapshot, err := c.leaseStatus(ctx, token)
		if err != nil {
			return board.Snapshot{}, err
		}
		if snapshot.Phase != board.Active && snapshot.Phase != board.YieldRequested && snapshot.Phase != board.Draining {
			return board.Snapshot{}, ErrStaleLease
		}
		var challenge store.NeutralChallenge
		err = c.request(ctx, http.MethodPost, boardPath(token.BoardID, "/neutral-challenge"), struct {
			ExpectedVersion uint64 `json:"expected_version"`
			Purpose         string `json:"purpose"`
		}{snapshot.Version, "release"}, &challenge)
		if isConflict(err) {
			if err := waitConflict(ctx); err != nil {
				return board.Snapshot{}, err
			}
			continue
		}
		if err != nil {
			return board.Snapshot{}, err
		}
		if !store.ValidID(challenge.ID) || challenge.BoardID != token.BoardID ||
			challenge.LeaseID != token.LeaseID || challenge.Generation != token.Generation ||
			challenge.SnapshotVersion != snapshot.Version || challenge.Purpose != "release" ||
			challenge.Nonce == "" || challenge.ProfileSHA256 == "" || challenge.FixtureRevision == "" ||
			!time.Now().Before(challenge.ExpiresAt) {
			return board.Snapshot{}, ErrInvalidNeutralProof
		}
		receipt, err := producer.ProduceNeutralReceipt(ctx, challenge)
		if err != nil {
			return board.Snapshot{}, err
		}
		if len(receipt) == 0 || len(receipt) > 65536 {
			return board.Snapshot{}, ErrInvalidNeutralProof
		}
		result, err := c.command(ctx, token.BoardID, "/leases/"+url.PathEscape(token.LeaseID)+"/free", struct {
			ExpectedVersion uint64 `json:"expected_version"`
			Generation      uint64 `json:"generation"`
			ChallengeID     string `json:"challenge_id"`
			Receipt         []byte `json:"receipt"`
		}{snapshot.Version, token.Generation, challenge.ID, receipt})
		if err == nil || !isConflict(err) {
			return result, err
		}
		if err := waitConflict(ctx); err != nil {
			return board.Snapshot{}, err
		}
	}
}
