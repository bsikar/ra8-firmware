package boardclient

import (
	"context"
	"errors"
	"net/http"
	"strings"
	"testing"
)

func TestEveryRequestCarriesAThreadTheServerWillEcho(t *testing.T) {
	var seen []string
	c, closeServer := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		seen = append(seen, r.Header.Get(correlationHeader))
		jsonResponse(w, http.StatusOK, activeBoard(t))
	})
	defer closeServer()

	for i := 0; i < 2; i++ {
		if _, err := c.Status(context.Background(), "ek-ra8d2"); err != nil {
			t.Fatalf("status %d: %v", i, err)
		}
	}
	if len(seen) != 2 {
		t.Fatalf("requests served: got %d, want 2", len(seen))
	}
	for i, id := range seen {
		if !validCorrelationID(id) {
			t.Fatalf("request %d carried an identifier the server would replace: %q", i, id)
		}
	}
	if seen[0] == seen[1] {
		t.Fatalf("two unpinned requests shared one thread: %q", seen[0])
	}
}

func TestAPinnedThreadCoversEveryRequestUnderIt(t *testing.T) {
	var seen []string
	c, closeServer := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		seen = append(seen, r.Header.Get(correlationHeader))
		jsonResponse(w, http.StatusOK, activeBoard(t))
	})
	defer closeServer()

	ctx, err := WithCorrelationID(context.Background(), "ci-job-4417.attempt-2")
	if err != nil {
		t.Fatalf("pinning a well-formed identifier: %v", err)
	}
	if got := CorrelationIDFrom(ctx); got != "ci-job-4417.attempt-2" {
		t.Fatalf("pinned identifier read back: got %q", got)
	}
	for i := 0; i < 3; i++ {
		if _, err := c.Status(ctx, "ek-ra8d2"); err != nil {
			t.Fatalf("status %d: %v", i, err)
		}
	}
	for i, id := range seen {
		if id != "ci-job-4417.attempt-2" {
			t.Fatalf("request %d left the pinned thread: %q", i, id)
		}
	}
}

func TestAnIdentifierTheServerWouldRefuseIsRefusedBeforeItIsSent(t *testing.T) {
	for name, id := range map[string]string{
		"empty":      "",
		"space":      "ci job 4417",
		"newline":    "ci-4417\nX-Correlation-Id: someone-elses",
		"nul":        "ci-4417\x00",
		"non ascii":  "ci-4417-\u00e9",
		"over bound": strings.Repeat("a", maxCorrelationID+1),
	} {
		t.Run(name, func(t *testing.T) {
			ctx, err := WithCorrelationID(context.Background(), id)
			if !errors.Is(err, ErrInvalidRequest) {
				t.Fatalf("pinning %q: got %v, want ErrInvalidRequest", id, err)
			}
			if got := CorrelationIDFrom(ctx); got != "" {
				t.Fatalf("a refused identifier was pinned anyway: %q", got)
			}
		})
	}
}

func TestARefusedPinLeavesTheRequestWithAMintedThread(t *testing.T) {
	var seen string
	c, closeServer := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		seen = r.Header.Get(correlationHeader)
		jsonResponse(w, http.StatusOK, activeBoard(t))
	})
	defer closeServer()

	ctx, err := WithCorrelationID(context.Background(), "ci 4417")
	if !errors.Is(err, ErrInvalidRequest) {
		t.Fatalf("pinning a malformed identifier: got %v", err)
	}
	if _, err := c.Status(ctx, "ek-ra8d2"); err != nil {
		t.Fatalf("status: %v", err)
	}
	if !validCorrelationID(seen) || seen == "ci 4417" {
		t.Fatalf("request thread after a refused pin: %q", seen)
	}
}

func TestAFailureCarriesTheThreadTheServerAnsweredWith(t *testing.T) {
	c, closeServer := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set(correlationHeader, "server-chose-this")
		jsonResponse(w, http.StatusServiceUnavailable, map[string]any{
			"code": "unavailable", "detail": "database operation unavailable",
			"retryable": true, "correlation_id": "server-chose-this",
		})
	})
	defer closeServer()

	_, err := c.Status(context.Background(), "ek-ra8d2")
	var httpErr *HTTPError
	if !errors.As(err, &httpErr) {
		t.Fatalf("status error: got %v, want *HTTPError", err)
	}
	if httpErr.CorrelationID != "server-chose-this" {
		t.Fatalf("thread on the failure: got %q, want server-chose-this", httpErr.CorrelationID)
	}
	if !strings.Contains(httpErr.Error(), "server-chose-this") {
		t.Fatalf("the operator cannot read the thread off the error: %s", httpErr.Error())
	}
}

func TestTheServersOwnThreadWinsOverTheOneThatWasSent(t *testing.T) {
	c, closeServer := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		// What a server does when it replaces a caller's identifier: the
		// answer names the thread it actually recorded under.
		w.Header().Set(correlationHeader, "replaced-by-the-server")
		jsonResponse(w, http.StatusConflict, map[string]any{
			"code": "conflict", "detail": "stale", "retryable": false,
		})
	})
	defer closeServer()

	ctx, err := WithCorrelationID(context.Background(), "sent-by-the-caller")
	if err != nil {
		t.Fatalf("pinning: %v", err)
	}
	_, err = c.Status(ctx, "ek-ra8d2")
	var httpErr *HTTPError
	if !errors.As(err, &httpErr) {
		t.Fatalf("status error: got %v, want *HTTPError", err)
	}
	if httpErr.CorrelationID != "replaced-by-the-server" {
		t.Fatalf("thread on the failure: got %q, want the server's own", httpErr.CorrelationID)
	}
}

func TestAThreadIsReadFromTheProblemBodyWhenTheHeaderIsGone(t *testing.T) {
	c, closeServer := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		jsonResponse(w, http.StatusServiceUnavailable, map[string]any{
			"code": "unavailable", "detail": "database operation unavailable",
			"retryable": true, "correlation_id": "only-in-the-body",
		})
	})
	defer closeServer()

	_, err := c.Status(context.Background(), "ek-ra8d2")
	var httpErr *HTTPError
	if !errors.As(err, &httpErr) {
		t.Fatalf("status error: got %v, want *HTTPError", err)
	}
	if httpErr.CorrelationID != "only-in-the-body" {
		t.Fatalf("thread on the failure: got %q, want only-in-the-body", httpErr.CorrelationID)
	}
}

func TestNoThreadIsInventedWhenTheServerNamedNone(t *testing.T) {
	c, closeServer := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		jsonResponse(w, http.StatusNotFound, map[string]any{
			"code": "not_found", "detail": "board not found", "retryable": false,
		})
	})
	defer closeServer()

	ctx, err := WithCorrelationID(context.Background(), "sent-by-the-caller")
	if err != nil {
		t.Fatalf("pinning: %v", err)
	}
	_, err = c.Status(ctx, "ek-ra8d2")
	var httpErr *HTTPError
	if !errors.As(err, &httpErr) {
		t.Fatalf("status error: got %v, want *HTTPError", err)
	}
	if httpErr.CorrelationID != "" {
		t.Fatalf("a thread was invented from what was sent: %q", httpErr.CorrelationID)
	}
	if strings.Contains(httpErr.Error(), "correlation") {
		t.Fatalf("the error claims a thread it does not have: %s", httpErr.Error())
	}
}

func TestAMalformedServedThreadIsNotReflected(t *testing.T) {
	c, closeServer := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		jsonResponse(w, http.StatusServiceUnavailable, map[string]any{
			"code": "unavailable", "detail": "unavailable", "retryable": true,
			"correlation_id": "not a valid thread",
		})
	})
	defer closeServer()

	_, err := c.Status(context.Background(), "ek-ra8d2")
	var httpErr *HTTPError
	if !errors.As(err, &httpErr) {
		t.Fatalf("status error: got %v, want *HTTPError", err)
	}
	if httpErr.CorrelationID != "" {
		t.Fatalf("a malformed served identifier was kept: %q", httpErr.CorrelationID)
	}
}
