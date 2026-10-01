// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package demand

import (
	"context"
	"crypto/hmac"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"io"
	"net/http"
	"strings"
	"time"
)

// WebhookAdapter names the GitHub App delivery path. Like every adapter name
// it is evidence about how a copy of the event arrived, never identity.
const WebhookAdapter = "github-app"

// signatureHeader, eventHeader and deliveryHeader are what a GitHub App
// delivery carries. The signature is over the exact bytes GitHub sent, so it
// has to be checked before anything parses them.
const (
	signatureHeader = "X-Hub-Signature-256"
	eventHeader     = "X-GitHub-Event"
	deliveryHeader  = "X-GitHub-Delivery"
	signaturePrefix = "sha256="
)

// minWebhookSecretBytes keeps a deployment from arming the endpoint with a
// secret short enough to guess. GitHub lets you choose the secret, so the
// only place this can be enforced is here.
const minWebhookSecretBytes = 16

// EventRecorder stores one normalized demand event. The store's dedupe is
// what makes at-least-once delivery safe, so the endpoint hands every
// accepted delivery over and never tries to filter duplicates itself.
type EventRecorder interface {
	Record(ctx context.Context, event Event) error
}

// WebhookConfig configures the GitHub App receiver.
type WebhookConfig struct {
	// Secret is the webhook secret configured on the GitHub App. An
	// endpoint without one would accept demand from anybody.
	Secret []byte
	// Recorder persists accepted deliveries.
	Recorder EventRecorder
	// Adapter defaults to WebhookAdapter.
	Adapter string
	// Now defaults to time.Now and supplies the observation time. A
	// sender must not be able to backdate demand, so the payload's own
	// timestamps never fill this in.
	Now func() time.Time
}

// Webhook is the workflow_job receiver. It answers GitHub the way GitHub
// reads answers: a 2xx means stop retrying, so anything this plane has
// durably taken or deliberately ignored is a 2xx, and only a failure that a
// retry could fix is a 5xx.
type Webhook struct {
	secret   []byte
	recorder EventRecorder
	adapter  string
	now      func() time.Time
}

// NewWebhook validates the configuration up front: an endpoint that came up
// without a secret is not one to discover at the first delivery.
func NewWebhook(config WebhookConfig) (*Webhook, error) {
	adapter := config.Adapter
	if adapter == "" {
		adapter = WebhookAdapter
	}
	now := config.Now
	if now == nil {
		now = time.Now
	}
	switch {
	case len(config.Secret) < minWebhookSecretBytes:
		return nil, errors.New("webhook secret must be at least 16 bytes")
	case config.Recorder == nil:
		return nil, errors.New("webhook requires an event recorder")
	case len(adapter) > 64:
		return nil, errors.New("webhook adapter name is too long")
	}
	secret := make([]byte, len(config.Secret))
	copy(secret, config.Secret)
	return &Webhook{secret: secret, recorder: config.Recorder, adapter: adapter, now: now}, nil
}

// signed reports whether header authenticates body under the shared secret.
// The comparison is constant time, and a malformed header is simply not a
// signature.
func (h *Webhook) signed(header string, body []byte) bool {
	if !strings.HasPrefix(header, signaturePrefix) {
		return false
	}
	presented, err := hex.DecodeString(strings.TrimPrefix(header, signaturePrefix))
	if err != nil || len(presented) != sha256.Size {
		return false
	}
	mac := hmac.New(sha256.New, h.secret)
	mac.Write(body)
	return hmac.Equal(presented, mac.Sum(nil))
}

func jsonContentType(value string) bool {
	media, _, _ := strings.Cut(value, ";")
	return strings.EqualFold(strings.TrimSpace(media), "application/json")
}

// ServeHTTP takes one delivery. The order is deliberate: reject what can be
// rejected without reading a body, read a bounded body, authenticate those
// exact bytes, and only then parse them.
func (h *Webhook) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	if h == nil || r == nil {
		http.Error(w, "webhook unavailable", http.StatusInternalServerError)
		return
	}
	if r.Method != http.MethodPost {
		w.Header().Set("Allow", http.MethodPost)
		http.Error(w, "webhook accepts POST", http.StatusMethodNotAllowed)
		return
	}
	if !jsonContentType(r.Header.Get("Content-Type")) {
		http.Error(w, "webhook accepts application/json", http.StatusUnsupportedMediaType)
		return
	}
	event := r.Header.Get(eventHeader)
	deliveryID := r.Header.Get(deliveryHeader)
	if !deliveryPattern.MatchString(deliveryID) {
		http.Error(w, "missing or malformed delivery id", http.StatusBadRequest)
		return
	}
	body, err := io.ReadAll(http.MaxBytesReader(w, r.Body, MaxPayloadBytes))
	if err != nil {
		http.Error(w, "delivery body exceeds the accepted size", http.StatusRequestEntityTooLarge)
		return
	}
	// Authenticate before parsing: an unverified sender must not be able
	// to reach the JSON decoder, and the signature covers these bytes.
	if !h.signed(r.Header.Get(signatureHeader), body) {
		http.Error(w, "delivery signature did not verify", http.StatusUnauthorized)
		return
	}
	// ping is how GitHub proves the endpoint is wired up. It carries no
	// demand, and answering it is the whole job.
	if event == "ping" {
		w.WriteHeader(http.StatusOK)
		return
	}
	if event != "workflow_job" {
		// A signed event this plane does not act on is accepted, not
		// retried: subscribing to more events is a GitHub-side change
		// and must not turn into a retry loop here.
		w.WriteHeader(http.StatusNoContent)
		return
	}
	normalized, err := NormalizeWorkflowJob(h.adapter, deliveryID, body, h.now())
	switch {
	case errors.Is(err, ErrIgnored):
		w.WriteHeader(http.StatusNoContent)
		return
	case err != nil:
		http.Error(w, "delivery is not a workflow_job this plane can read", http.StatusBadRequest)
		return
	}
	if err := h.recorder.Record(r.Context(), normalized); err != nil {
		// The delivery was well formed and authentic; this plane just
		// could not store it. A 5xx is what makes GitHub replay it,
		// and dedupe on the job and attempt makes that replay safe.
		http.Error(w, "could not record demand", http.StatusServiceUnavailable)
		return
	}
	w.WriteHeader(http.StatusAccepted)
}

// SignDelivery produces the header value GitHub would send for body under
// secret. It exists so tests and local tooling sign deliveries the same way
// the endpoint verifies them, rather than each reimplementing the scheme.
func SignDelivery(secret, body []byte) string {
	mac := hmac.New(sha256.New, secret)
	mac.Write(body)
	return signaturePrefix + hex.EncodeToString(mac.Sum(nil))
}
