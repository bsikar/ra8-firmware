// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

// Package runclient submits and observes server-owned runs over mTLS.
package runclient

import (
	"bytes"
	"context"
	"crypto/sha256"
	"crypto/tls"
	"crypto/x509"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"os"
	"strings"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

const maxResponseBytes = 1 << 20

// Config contains the server endpoint and the caller's mutually authenticated identity.
type Config struct {
	ServerURL string
	CAFile    string
	CertFile  string
	KeyFile   string
}

// Source identifies the exact repository revision and clean snapshot to execute.
type Source struct {
	Repository     string `json:"repo"`
	Branch         string `json:"branch"`
	CommitSHA      string `json:"commit"`
	SnapshotSHA256 string `json:"snapshot_sha256"`
}

// Task requests one catalog task and its dependencies within a run.
//
// Values carries the task's arguments BY NAME. Args stays on the wire for the
// older shape and must be empty: the plane binds argv itself from Values and
// its own reviewed schema, so a submitter never states argv.
type Task struct {
	Key           string            `json:"key"`
	Name          string            `json:"name"`
	Args          []string          `json:"args"`
	Values        map[string]string `json:"values,omitempty"`
	DependsOnKeys []string          `json:"depends_on_keys"`
}

// SubmitRequest is the immutable, idempotent run admission payload.
type SubmitRequest struct {
	Trigger       string `json:"trigger"`
	Source        Source `json:"source"`
	CatalogDigest string `json:"catalog_digest"`
	Tasks         []Task `json:"tasks"`
}

// Receipt identifies a newly admitted or idempotently replayed run.
type Receipt struct {
	ID    string `json:"id"`
	State string `json:"state"`
}

// Client holds an HTTPS-origin-only transport with redirects disabled.
type Client struct {
	base *url.URL
	http *http.Client
}

// New loads the supplied trust roots and client key pair, and builds a TLS 1.3 client.
func New(config Config) (*Client, error) {
	base, err := url.Parse(config.ServerURL)
	if err != nil || base.Scheme != "https" || base.Host == "" || base.User != nil ||
		(base.Path != "" && base.Path != "/") || base.RawQuery != "" || base.Fragment != "" ||
		config.CAFile == "" || config.CertFile == "" || config.KeyFile == "" {
		return nil, errors.New("run client requires an HTTPS origin and mTLS identity")
	}
	caPEM, err := os.ReadFile(config.CAFile)
	if err != nil {
		return nil, fmt.Errorf("read server CA: %w", err)
	}
	roots := x509.NewCertPool()
	if !roots.AppendCertsFromPEM(caPEM) {
		return nil, errors.New("server CA has no trusted certificate")
	}
	identity, err := tls.LoadX509KeyPair(config.CertFile, config.KeyFile)
	if err != nil {
		return nil, fmt.Errorf("load run client identity: %w", err)
	}
	base.Path = ""
	base.RawPath = ""
	transport := &http.Transport{Proxy: nil, ForceAttemptHTTP2: true, TLSClientConfig: &tls.Config{
		MinVersion: tls.VersionTLS13, RootCAs: roots, Certificates: []tls.Certificate{identity},
	}}
	return &Client{base: base, http: &http.Client{Transport: transport, Timeout: 30 * time.Second,
		CheckRedirect: func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }}}, nil
}

func (c *Client) Close() {
	if c != nil && c.http != nil {
		c.http.CloseIdleConnections()
	}
}

func validIdempotencyKey(key string) bool {
	if len(key) == 0 || len(key) > 256 || strings.TrimSpace(key) != key {
		return false
	}
	for _, value := range key {
		if value < 0x21 || value > 0x7e {
			return false
		}
	}
	return true
}

// Submit admits a bounded run using a required idempotency key.
func (c *Client) Submit(ctx context.Context, key string, input SubmitRequest) (Receipt, error) {
	if !validIdempotencyKey(key) || len(input.Tasks) == 0 || len(input.Tasks) > 100 {
		return Receipt{}, errors.New("run submission requires a valid idempotency key and 1..100 tasks")
	}
	body, err := json.Marshal(input)
	if err != nil {
		return Receipt{}, fmt.Errorf("encode run submission: %w", err)
	}
	if len(body) > maxResponseBytes {
		return Receipt{}, errors.New("run submission exceeds request limit")
	}
	var receipt Receipt
	if err := c.do(ctx, http.MethodPost, "/v1/runs", key, body, &receipt); err != nil {
		return Receipt{}, err
	}
	if !store.ValidID(receipt.ID) || receipt.State != "queued" {
		return Receipt{}, errors.New("run submission returned an invalid receipt")
	}
	return receipt, nil
}

// Get fetches an authorized run and confirms the response belongs to the requested ID.
func (c *Client) Get(ctx context.Context, id string) (store.Run, error) {
	if !store.ValidID(id) {
		return store.Run{}, errors.New("invalid run ID")
	}
	var result store.Run
	if err := c.do(ctx, http.MethodGet, "/v1/runs/"+url.PathEscape(id), "", nil, &result); err != nil {
		return store.Run{}, err
	}
	if result.ID != id || !store.ValidID(result.ID) || result.State == "" {
		return store.Run{}, errors.New("run status response does not match requested ID")
	}
	return result, nil
}

// Events returns one contiguous, bounded page of immutable run events.
func (c *Client) Events(ctx context.Context, runID string, after int64, limit int) (store.RunEventPage, error) {
	if !store.ValidID(runID) || after < 0 || limit < 1 || limit > store.MaxEventPageSize {
		return store.RunEventPage{}, errors.New("invalid run event page request")
	}
	var page store.RunEventPage
	if err := c.do(ctx, http.MethodGet, "/v1/runs/"+url.PathEscape(runID)+"/events", "", nil, &page, url.Values{
		"after": {fmt.Sprint(after)}, "limit": {fmt.Sprint(limit)},
	}); err != nil {
		return store.RunEventPage{}, err
	}
	if page.RunID != runID || len(page.Events) > limit || page.NextAfter < after ||
		page.NextAfter-after > int64(limit) || (page.HasMore && len(page.Events) != limit) {
		return store.RunEventPage{}, errors.New("run event page does not match request")
	}
	sequence := after
	for _, event := range page.Events {
		var object map[string]json.RawMessage
		if event.Sequence != sequence+1 || !store.ValidID(event.ID) || event.Kind == "" || event.HappenedAt.IsZero() ||
			len(event.Data) < 2 || json.Unmarshal(event.Data, &object) != nil || object == nil {
			return store.RunEventPage{}, errors.New("run event page contains an invalid event")
		}
		sequence = event.Sequence
	}
	if page.NextAfter != sequence || (len(page.Events) == 0 && page.HasMore) {
		return store.RunEventPage{}, errors.New("run event cursor is inconsistent")
	}
	return page, nil
}

// Cancel requests idempotent, cooperative cancellation and verifies the server
// response belongs to the requested run. Active tasks stop at their next
// authenticated heartbeat and must still submit terminal evidence.
func (c *Client) Cancel(ctx context.Context, id string) (store.Run, error) {
	if !store.ValidID(id) {
		return store.Run{}, errors.New("invalid run ID")
	}
	var result store.Run
	if err := c.do(ctx, http.MethodPost, "/v1/runs/"+url.PathEscape(id)+"/cancel", "", nil, &result); err != nil {
		return store.Run{}, err
	}
	if result.ID != id || !store.ValidID(result.ID) || result.State == "" ||
		(result.CancelRequestedAt == nil && result.State != "terminal") {
		return store.Run{}, errors.New("run cancellation response does not match requested run")
	}
	return result, nil
}

// Logs returns one bounded page and verifies every chunk digest before exposing it.
func (c *Client) Logs(ctx context.Context, runID, attemptID string, after int64, limit int) (store.LogPage, error) {
	if !store.ValidID(runID) || !store.ValidID(attemptID) || after < 0 || limit < 1 || limit > store.MaxLogPageSize {
		return store.LogPage{}, errors.New("invalid run log page request")
	}
	endpoint := "/v1/runs/" + url.PathEscape(runID) + "/logs"
	var page store.LogPage
	if err := c.do(ctx, http.MethodGet, endpoint, "", nil, &page, url.Values{
		"attempt_id": {attemptID}, "after": {fmt.Sprint(after)}, "limit": {fmt.Sprint(limit)},
	}); err != nil {
		return store.LogPage{}, err
	}
	if page.AttemptID != attemptID || len(page.Chunks) > limit || page.NextAfter < after ||
		page.NextAfter-after > int64(limit) {
		return store.LogPage{}, errors.New("run log response does not match request")
	}
	expectedSequence := after
	for index, chunk := range page.Chunks {
		data, err := base64.StdEncoding.DecodeString(chunk.DataBase64)
		if err != nil || len(data) == 0 || len(data) > 65536 || chunk.Sequence != expectedSequence+1 ||
			(chunk.Stream != "stdout" && chunk.Stream != "stderr") {
			return store.LogPage{}, errors.New("run log response has an invalid chunk")
		}
		sum := sha256.Sum256(data)
		if hex.EncodeToString(sum[:]) != chunk.SHA256 {
			return store.LogPage{}, errors.New("run log chunk digest mismatch")
		}
		page.Chunks[index].Data = data
		expectedSequence = chunk.Sequence
	}
	if page.NextAfter != expectedSequence || (page.HasMore && len(page.Chunks) != limit) {
		return store.LogPage{}, errors.New("run log pagination cursor is inconsistent")
	}
	return page, nil
}

func (c *Client) do(ctx context.Context, method, path, idempotencyKey string, body []byte, output any, queries ...url.Values) error {
	if c == nil || c.base == nil || c.http == nil {
		return errors.New("run client is not configured")
	}
	endpoint := *c.base
	endpoint.Path = path
	if len(queries) > 1 {
		return errors.New("run API received multiple query sets")
	}
	if len(queries) == 1 {
		endpoint.RawQuery = queries[0].Encode()
	}
	request, err := http.NewRequestWithContext(ctx, method, endpoint.String(), bytes.NewReader(body))
	if err != nil {
		return err
	}
	if body != nil {
		request.Header.Set("Content-Type", "application/json")
	}
	if idempotencyKey != "" {
		request.Header.Set("Idempotency-Key", idempotencyKey)
	}
	response, err := c.http.Do(request)
	if err != nil {
		return err
	}
	defer response.Body.Close()
	raw, err := io.ReadAll(io.LimitReader(response.Body, maxResponseBytes+1))
	if err != nil || len(raw) > maxResponseBytes {
		return errors.New("run API response is unreadable or exceeds limit")
	}
	if response.StatusCode < 200 || response.StatusCode >= 300 {
		return fmt.Errorf("run API returned HTTP %d", response.StatusCode)
	}
	if output != nil {
		decoder := json.NewDecoder(bytes.NewReader(raw))
		decoder.DisallowUnknownFields()
		if err := decoder.Decode(output); err != nil {
			return fmt.Errorf("decode run API response: %w", err)
		}
		var trailing any
		if err := decoder.Decode(&trailing); !errors.Is(err, io.EOF) {
			return errors.New("run API response contains trailing JSON")
		}
	}
	return nil
}
