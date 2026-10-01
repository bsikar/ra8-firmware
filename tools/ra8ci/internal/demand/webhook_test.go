// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package demand

import (
	"context"
	"errors"
	"fmt"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"
)

var webhookSecret = []byte("a-webhook-secret-long-enough")

var webhookNow = time.Date(2026, 9, 24, 12, 30, 0, 0, time.UTC)

type recordingStore struct {
	events []Event
	err    error
}

func (r *recordingStore) Record(ctx context.Context, event Event) error {
	if r.err != nil {
		return r.err
	}
	r.events = append(r.events, event)
	return nil
}

func webhookFor(t *testing.T, recorder EventRecorder) *Webhook {
	t.Helper()
	hook, err := NewWebhook(WebhookConfig{Secret: webhookSecret, Recorder: recorder,
		Now: func() time.Time { return webhookNow }})
	if err != nil {
		t.Fatalf("NewWebhook: %v", err)
	}
	return hook
}

func workflowJobBody(action, status, conclusion string) string {
	conclusionField := "null"
	completed := "null"
	if conclusion != "" {
		conclusionField = fmt.Sprintf("%q", conclusion)
		completed = `"2026-09-24T12:20:00Z"`
	}
	return fmt.Sprintf(`{"action":%q,"workflow_job":{"id":4242,"run_id":99,"run_attempt":1,
		"name":"build","workflow_name":"ci","head_sha":%q,"status":%q,"conclusion":%s,
		"labels":["self-hosted","ra8"],"runner_name":"ra8-runner-3",
		"created_at":"2026-09-24T12:00:00Z","started_at":"2026-09-24T12:05:00Z",
		"completed_at":%s},"repository":{"name":"ra8-firmware","full_name":"bsikar/ra8-firmware",
		"owner":{"login":"bsikar"}}}`, action, strings.Repeat("b", 40), status, conclusionField, completed)
}

func delivery(t *testing.T, hook *Webhook, event, deliveryID, body string, mutate ...func(*http.Request)) *httptest.ResponseRecorder {
	t.Helper()
	request := httptest.NewRequest(http.MethodPost, "/webhooks/github", strings.NewReader(body))
	request.Header.Set("Content-Type", "application/json")
	request.Header.Set(eventHeader, event)
	request.Header.Set(deliveryHeader, deliveryID)
	request.Header.Set(signatureHeader, SignDelivery(webhookSecret, []byte(body)))
	for _, apply := range mutate {
		apply(request)
	}
	response := httptest.NewRecorder()
	hook.ServeHTTP(response, request)
	return response
}

func TestWebhookAcceptsASignedQueuedJob(t *testing.T) {
	store := &recordingStore{}
	body := workflowJobBody("queued", "queued", "")

	response := delivery(t, webhookFor(t, store), "workflow_job", "1234-abcd", body)

	if response.Code != http.StatusAccepted {
		t.Fatalf("status %d, want 202: %s", response.Code, response.Body)
	}
	if len(store.events) != 1 {
		t.Fatalf("recorded %d events, want 1", len(store.events))
	}
	got := store.events[0]
	if got.Key() != "4242/1" || got.Phase != PhaseQueued {
		t.Errorf("unexpected event identity: %+v", got)
	}
	if got.Adapter != WebhookAdapter || got.DeliveryID != "1234-abcd" {
		t.Errorf("adapter %q delivery %q", got.Adapter, got.DeliveryID)
	}
	if !got.ObservedAt.Equal(webhookNow) {
		t.Errorf("observed at %s, want the server clock %s", got.ObservedAt, webhookNow)
	}
}

// The signature is the only thing standing between this endpoint and anyone
// who can reach it, so every way of getting it wrong has to be a 401 and
// nothing may be recorded.
func TestWebhookRefusesUnauthenticatedDeliveries(t *testing.T) {
	body := workflowJobBody("queued", "queued", "")
	cases := map[string]func(*http.Request){
		"no signature":    func(r *http.Request) { r.Header.Del(signatureHeader) },
		"empty signature": func(r *http.Request) { r.Header.Set(signatureHeader, "") },
		"no prefix": func(r *http.Request) {
			r.Header.Set(signatureHeader, strings.TrimPrefix(SignDelivery(webhookSecret, []byte(body)), signaturePrefix))
		},
		"sha1 prefix": func(r *http.Request) { r.Header.Set(signatureHeader, "sha1=deadbeef") },
		"not hex":     func(r *http.Request) { r.Header.Set(signatureHeader, signaturePrefix+strings.Repeat("z", 64)) },
		"truncated":   func(r *http.Request) { r.Header.Set(signatureHeader, SignDelivery(webhookSecret, []byte(body))[:40]) },
		"another secret": func(r *http.Request) {
			r.Header.Set(signatureHeader, SignDelivery([]byte("some-other-secret-value"), []byte(body)))
		},
		"signs other bytes": func(r *http.Request) { r.Header.Set(signatureHeader, SignDelivery(webhookSecret, []byte(body+" "))) },
	}
	for name, mutate := range cases {
		store := &recordingStore{}
		response := delivery(t, webhookFor(t, store), "workflow_job", "1234-abcd", body, mutate)
		if response.Code != http.StatusUnauthorized {
			t.Errorf("%s: status %d, want 401", name, response.Code)
		}
		if len(store.events) != 0 {
			t.Errorf("%s: recorded %d events on an unauthenticated delivery", name, len(store.events))
		}
	}
}

// A body that does not match its signature is a tampered delivery, and the
// check has to be over the exact bytes rather than anything parsed out.
func TestWebhookRefusesATamperedBody(t *testing.T) {
	signedBody := workflowJobBody("queued", "queued", "")
	tampered := strings.Replace(signedBody, `"id":4242`, `"id":9999`, 1)
	store := &recordingStore{}

	request := httptest.NewRequest(http.MethodPost, "/webhooks/github", strings.NewReader(tampered))
	request.Header.Set("Content-Type", "application/json")
	request.Header.Set(eventHeader, "workflow_job")
	request.Header.Set(deliveryHeader, "1234-abcd")
	request.Header.Set(signatureHeader, SignDelivery(webhookSecret, []byte(signedBody)))
	response := httptest.NewRecorder()
	webhookFor(t, store).ServeHTTP(response, request)

	if response.Code != http.StatusUnauthorized || len(store.events) != 0 {
		t.Fatalf("status %d, recorded %d: a tampered body was taken", response.Code, len(store.events))
	}
}

// GitHub retries anything that is not a 2xx, so a signed event this plane
// deliberately does not act on must not turn into a retry loop.
func TestWebhookAcceptsWithoutActingOnEventsItIgnores(t *testing.T) {
	cases := map[string]struct {
		event string
		body  string
		want  int
	}{
		"ping":           {"ping", `{"zen":"anything"}`, http.StatusOK},
		"other event":    {"push", `{"ref":"refs/heads/dev"}`, http.StatusNoContent},
		"waiting action": {"workflow_job", workflowJobBody("waiting", "waiting", ""), http.StatusNoContent},
		"unknown action": {"workflow_job", workflowJobBody("whatever", "whatever", ""), http.StatusNoContent},
	}
	for name, test := range cases {
		store := &recordingStore{}
		response := delivery(t, webhookFor(t, store), test.event, "1234-abcd", test.body)
		if response.Code != test.want {
			t.Errorf("%s: status %d, want %d", name, response.Code, test.want)
		}
		if len(store.events) != 0 {
			t.Errorf("%s: recorded an event it should have ignored", name)
		}
	}
}

// A delivery that verifies but does not describe a state this plane can read
// is the sender's problem, and retrying it would never help.
func TestWebhookRejectsMalformedDeliveries(t *testing.T) {
	cases := map[string]string{
		"not json":            `{"action":`,
		"no action":           `{"workflow_job":{"id":1}}`,
		"status disagrees":    workflowJobBody("queued", "in_progress", ""),
		"repository mismatch": strings.Replace(workflowJobBody("queued", "queued", ""), `"full_name":"bsikar/ra8-firmware"`, `"full_name":"someone/else"`, 1),
		"no labels":           strings.Replace(workflowJobBody("queued", "queued", ""), `"labels":["self-hosted","ra8"]`, `"labels":[]`, 1),
	}
	for name, body := range cases {
		store := &recordingStore{}
		response := delivery(t, webhookFor(t, store), "workflow_job", "1234-abcd", body)
		if response.Code != http.StatusBadRequest {
			t.Errorf("%s: status %d, want 400", name, response.Code)
		}
		if len(store.events) != 0 {
			t.Errorf("%s: recorded a malformed delivery", name)
		}
	}
}

// A store that cannot take the write has to produce a retryable answer: the
// dedupe on job and attempt is what makes GitHub's replay safe.
func TestWebhookAsksForARetryWhenTheStoreFails(t *testing.T) {
	store := &recordingStore{err: errors.New("connection refused")}

	response := delivery(t, webhookFor(t, store), "workflow_job", "1234-abcd", workflowJobBody("queued", "queued", ""))

	if response.Code != http.StatusServiceUnavailable {
		t.Fatalf("status %d, want 503 so GitHub replays the delivery", response.Code)
	}
}

// At-least-once is the contract: the endpoint hands every authentic delivery
// to the store and never tries to filter replays itself.
func TestWebhookPassesReplaysStraightThrough(t *testing.T) {
	store := &recordingStore{}
	hook := webhookFor(t, store)
	body := workflowJobBody("queued", "queued", "")

	for _, id := range []string{"delivery-1", "delivery-1", "delivery-2"} {
		if response := delivery(t, hook, "workflow_job", id, body); response.Code != http.StatusAccepted {
			t.Fatalf("delivery %s: status %d", id, response.Code)
		}
	}
	if len(store.events) != 3 {
		t.Fatalf("recorded %d events, want all 3 handed to the store", len(store.events))
	}
	for _, event := range store.events {
		if event.Key() != "4242/1" {
			t.Errorf("replay landed on key %q: dedupe must happen on the unit of demand", event.Key())
		}
	}
}

func TestWebhookRejectsWhatItCannotEvenRead(t *testing.T) {
	store := &recordingStore{}
	hook := webhookFor(t, store)
	body := workflowJobBody("queued", "queued", "")

	get := httptest.NewRequest(http.MethodGet, "/webhooks/github", nil)
	getResponse := httptest.NewRecorder()
	hook.ServeHTTP(getResponse, get)
	if getResponse.Code != http.StatusMethodNotAllowed || getResponse.Header().Get("Allow") != http.MethodPost {
		t.Errorf("GET: status %d allow %q", getResponse.Code, getResponse.Header().Get("Allow"))
	}

	form := delivery(t, hook, "workflow_job", "1234-abcd", body, func(r *http.Request) {
		r.Header.Set("Content-Type", "application/x-www-form-urlencoded")
	})
	if form.Code != http.StatusUnsupportedMediaType {
		t.Errorf("form content type: status %d, want 415", form.Code)
	}

	charset := delivery(t, hook, "workflow_job", "1234-abcd", body, func(r *http.Request) {
		r.Header.Set("Content-Type", "application/json; charset=utf-8")
	})
	if charset.Code != http.StatusAccepted {
		t.Errorf("charset parameter: status %d, want 202", charset.Code)
	}

	for _, id := range []string{"", "has spaces", strings.Repeat("d", 200)} {
		response := delivery(t, hook, "workflow_job", "1234-abcd", body, func(r *http.Request) {
			r.Header.Set(deliveryHeader, id)
		})
		if response.Code != http.StatusBadRequest {
			t.Errorf("delivery id %q: status %d, want 400", id, response.Code)
		}
	}
}

// An unverified sender must not be able to make this process read an
// unbounded body, so the limit applies before the signature check.
func TestWebhookBoundsTheBody(t *testing.T) {
	store := &recordingStore{}
	oversized := `{"action":"queued","padding":"` + strings.Repeat("x", MaxPayloadBytes) + `"}`

	response := delivery(t, webhookFor(t, store), "workflow_job", "1234-abcd", oversized)

	if response.Code != http.StatusRequestEntityTooLarge {
		t.Fatalf("status %d, want 413", response.Code)
	}
	if len(store.events) != 0 {
		t.Fatal("recorded an oversized delivery")
	}
}

func TestNewWebhookRefusesAnUnarmedEndpoint(t *testing.T) {
	if _, err := NewWebhook(WebhookConfig{Recorder: &recordingStore{}}); err == nil {
		t.Error("a webhook with no secret was accepted")
	}
	if _, err := NewWebhook(WebhookConfig{Secret: []byte("short"), Recorder: &recordingStore{}}); err == nil {
		t.Error("a webhook with a guessable secret was accepted")
	}
	if _, err := NewWebhook(WebhookConfig{Secret: webhookSecret}); err == nil {
		t.Error("a webhook with no recorder was accepted")
	}
	hook, err := NewWebhook(WebhookConfig{Secret: webhookSecret, Recorder: &recordingStore{}})
	if err != nil || hook.adapter != WebhookAdapter || hook.now == nil {
		t.Fatalf("valid configuration: %v %+v", err, hook)
	}
}

// The secret is copied on construction: a caller reusing its buffer must not
// be able to disarm a running endpoint.
func TestNewWebhookCopiesTheSecret(t *testing.T) {
	secret := append([]byte(nil), webhookSecret...)
	hook, err := NewWebhook(WebhookConfig{Secret: secret, Recorder: &recordingStore{}})
	if err != nil {
		t.Fatalf("NewWebhook: %v", err)
	}
	clear(secret)
	if !hook.signed(SignDelivery(webhookSecret, []byte("body")), []byte("body")) {
		t.Fatal("clearing the caller's buffer changed the endpoint's secret")
	}
}
