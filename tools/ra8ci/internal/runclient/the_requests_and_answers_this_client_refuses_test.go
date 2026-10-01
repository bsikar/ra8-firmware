// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package runclient

import (
	"context"
	"encoding/base64"
	"net/http"
	"net/http/httptest"
	"net/url"
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// Everything here is what this client refuses without the plane's help: a
// request it will not send, a page whose own arithmetic does not hold, and
// an answer it cannot read. A control plane that trusted any of these
// would report a run it never actually saw.

// sending answers every request with the supplied handler.
func sending(t *testing.T, handler http.HandlerFunc) *Client {
	t.Helper()
	server := httptest.NewTLSServer(handler)
	t.Cleanup(server.Close)
	return testClient(server)
}

// refused runs a log request against a page the server would serve and
// reports whether the client kept it.
func refused(t *testing.T, page store.LogPage, limit int, after int64) error {
	t.Helper()
	_, err := serving(t, page).Logs(context.Background(), offsetRunID, offsetAttemptID, after, limit)
	return err
}

// An idempotency key is what stops a retried submission becoming a second
// run, so a key the server could read two ways is refused before a request
// is spent.
func TestAnIdempotencyKeyTheServerCouldReadTwoWaysIsRefused(t *testing.T) {
	for name, key := range map[string]string{
		"empty":          "",
		"leading space":  " retry-7",
		"trailing space": "retry-7 ",
		"inner space":    "retry 7",
		"tab":            "retry\t7",
		"newline":        "retry-7\n",
		"delete":         "retry-\x7f",
		"non-ASCII":      "retry-\u00e9",
		"over the bound": strings.Repeat("k", 257),
	} {
		if validIdempotencyKey(key) {
			t.Fatalf("%s key %q was accepted", name, key)
		}
	}
	for _, key := range []string{"k", "retry-7", strings.Repeat("k", 256), "!~"} {
		if !validIdempotencyKey(key) {
			t.Fatalf("key %q was refused", key)
		}
	}
}

// A submission larger than the plane will read is refused here rather than
// sent and rejected, because a request that big costs the server the read.
func TestASubmissionLargerThanThePlaneWillReadIsNotSent(t *testing.T) {
	asked := false
	client := sending(t, func(http.ResponseWriter, *http.Request) { asked = true })

	input := submissionInput()
	input.Tasks = nil
	for index := 0; index < 100; index++ {
		input.Tasks = append(input.Tasks, Task{
			Key: "task-001", Name: "test-go", Args: []string{strings.Repeat("a", 20000)},
		})
	}
	if _, err := client.Submit(context.Background(), "retry-7", input); err == nil ||
		!strings.Contains(err.Error(), "exceeds request limit") {
		t.Fatalf("an oversized submission answered %v", err)
	}
	if asked {
		t.Fatal("the oversized submission was sent anyway")
	}
}

// A refusal from the plane is carried back by every door, rather than read
// as an empty answer.
func TestARefusalFromThePlaneIsCarriedBackByEveryDoor(t *testing.T) {
	client := sending(t, func(w http.ResponseWriter, _ *http.Request) {
		w.WriteHeader(http.StatusServiceUnavailable)
	})
	ctx := context.Background()

	if _, err := client.Submit(ctx, "retry-7", submissionInput()); err == nil ||
		!strings.Contains(err.Error(), "HTTP 503") {
		t.Fatalf("submit answered %v", err)
	}
	if _, err := client.Logs(ctx, offsetRunID, offsetAttemptID, 0, 8); err == nil ||
		!strings.Contains(err.Error(), "HTTP 503") {
		t.Fatalf("logs answered %v", err)
	}
}

// An identifier the plane would not recognise is refused before a request
// is made, on every door that takes one.
func TestAnIdentifierThePlaneWouldNotRecogniseNeverLeavesTheClient(t *testing.T) {
	asked := false
	client := sending(t, func(http.ResponseWriter, *http.Request) { asked = true })
	ctx := context.Background()

	for _, id := range []string{"", "not-an-id", "00000000-0000-4000-8000-000000000001", offsetRunID + "x"} {
		if _, err := client.Get(ctx, id); err == nil {
			t.Fatalf("Get took %q", id)
		}
		if _, err := client.Cancel(ctx, id); err == nil {
			t.Fatalf("Cancel took %q", id)
		}
	}
	if asked {
		t.Fatal("a malformed identifier was sent to the plane")
	}
}

// A cancellation the plane did not stamp, on a run it says is still live,
// is refused: an operator who asked for a stop must not be told it took.
func TestACancellationWithNoStampOnALiveRunIsRefused(t *testing.T) {
	for _, state := range []string{"queued", "running"} {
		client := sending(t, func(w http.ResponseWriter, _ *http.Request) {
			_, _ = w.Write([]byte(`{"id":"` + answeredRunID + `","state":"` + state + `"}`))
		})
		if _, err := client.Cancel(context.Background(), answeredRunID); err == nil ||
			!strings.Contains(err.Error(), "no cancellation and no closed run") {
			t.Fatalf("state %q answered %v", state, err)
		}
	}
	client := sending(t, func(w http.ResponseWriter, _ *http.Request) {
		_, _ = w.Write([]byte(`{"id":"` + answeredRunID + `","state":"terminal"}`))
	})
	if _, err := client.Cancel(context.Background(), answeredRunID); err != nil {
		t.Fatalf("a closed run with no stamp was refused: %v", err)
	}
}

// A log page request the client cannot make is refused before the plane is
// asked, including the page size the store itself will not serve.
func TestALogPageRequestThisClientCannotMakeIsRefused(t *testing.T) {
	asked := false
	client := sending(t, func(http.ResponseWriter, *http.Request) { asked = true })
	ctx := context.Background()

	for name, ask := range map[string]struct {
		run, attempt string
		after        int64
		limit        int
	}{
		"no run":          {"", offsetAttemptID, 0, 8},
		"no attempt":      {offsetRunID, "", 0, 8},
		"negative cursor": {offsetRunID, offsetAttemptID, -1, 8},
		"no page":         {offsetRunID, offsetAttemptID, 0, 0},
		"negative page":   {offsetRunID, offsetAttemptID, 0, -1},
		"over the store":  {offsetRunID, offsetAttemptID, 0, store.MaxLogPageSize + 1},
	} {
		if _, err := client.Logs(ctx, ask.run, ask.attempt, ask.after, ask.limit); err == nil {
			t.Fatalf("%s was sent", name)
		}
	}
	if asked {
		t.Fatal("an unmakeable log request reached the plane")
	}
}

// A page that does not answer the request it was given is refused whole:
// a foreign attempt, more chunks than asked for, or a cursor that moved
// somewhere the request never reached.
func TestALogPageThatDoesNotAnswerTheRequestIsRefused(t *testing.T) {
	foreign := stamped(0, 0)
	foreign.AttemptID = offsetRunID
	if err := refused(t, foreign, 8, 0); err == nil ||
		!strings.Contains(err.Error(), "does not match request") {
		t.Fatalf("a foreign attempt answered %v", err)
	}

	overflowing := stamped(0, 0, 0)
	if err := refused(t, overflowing, 1, 0); err == nil ||
		!strings.Contains(err.Error(), "does not match request") {
		t.Fatalf("an overfull page answered %v", err)
	}

	backwards := stamped(5, 0)
	backwards.NextAfter = 4
	if err := refused(t, backwards, 8, 5); err == nil ||
		!strings.Contains(err.Error(), "does not match request") {
		t.Fatalf("a backwards cursor answered %v", err)
	}

	leaping := stamped(0, 0)
	leaping.NextAfter = 40
	if err := refused(t, leaping, 8, 0); err == nil ||
		!strings.Contains(err.Error(), "does not match request") {
		t.Fatalf("a leaping cursor answered %v", err)
	}
}

// A chunk the attempt could not have written is refused before its bytes
// are handed to a caller, and the digest check never gets to speak for it.
func TestAChunkTheAttemptCouldNotHaveWrittenIsRefused(t *testing.T) {
	undecodable := stamped(0, 0)
	undecodable.Chunks[0].DataBase64 = "not base64!!"

	empty := stamped(0, 0)
	empty.Chunks[0].DataBase64 = ""

	outOfOrder := stamped(0, 0, 0)
	outOfOrder.Chunks[1].Sequence = 7

	unknownStream := stamped(0, 0)
	unknownStream.Chunks[0].Stream = "stdlog"

	oversized := stamped(0, 0)
	big := strings.Repeat("x", 65537)
	oversized.Chunks[0].DataBase64 = base64.StdEncoding.EncodeToString([]byte(big))

	for name, page := range map[string]store.LogPage{
		"undecodable":    undecodable,
		"empty":          empty,
		"out of order":   outOfOrder,
		"unknown stream": unknownStream,
		"oversized":      oversized,
	} {
		err := refused(t, page, 8, 0)
		if err == nil || !strings.Contains(err.Error(), "invalid chunk") {
			t.Fatalf("%s chunk answered %v", name, err)
		}
	}
}

// A page claiming more behind it while handing back less than a full page
// is an inconsistent cursor: following it would skip whatever sits between.
func TestAPageClaimingMoreWhileHandingBackLessIsRefused(t *testing.T) {
	short := stamped(0, 0)
	short.HasMore = true

	if err := refused(t, short, 8, 0); err == nil ||
		!strings.Contains(err.Error(), "pagination cursor is inconsistent") {
		t.Fatalf("a short page claiming more answered %v", err)
	}
}

// A client that was never built answers rather than crashing, and a
// request this client cannot even form is refused before it is sent.
func TestARequestThisClientCannotFormIsRefusedBeforeItIsSent(t *testing.T) {
	var unbuilt *Client
	if err := unbuilt.do(context.Background(), http.MethodGet, "/v1/runs", "", nil, nil); err == nil ||
		!strings.Contains(err.Error(), "not configured") {
		t.Fatalf("an unbuilt client answered %v", err)
	}
	if err := (&Client{}).do(context.Background(), http.MethodGet, "/v1/runs", "", nil, nil); err == nil {
		t.Fatal("a half-built client made a request")
	}

	client := sending(t, func(http.ResponseWriter, *http.Request) {})
	if err := client.do(context.Background(), http.MethodGet, "/v1/runs", "", nil, nil,
		url.Values{"a": {"1"}}, url.Values{"b": {"2"}}); err == nil ||
		!strings.Contains(err.Error(), "multiple query sets") {
		t.Fatalf("two query sets answered %v", err)
	}
	if err := client.do(context.Background(), "BAD METHOD", "/v1/runs", "", nil, nil); err == nil {
		t.Fatal("an unformable method was sent")
	}
}

// An origin that answers nothing is an error, not an empty run.
func TestAnOriginThatAnswersNothingIsAnError(t *testing.T) {
	server := httptest.NewTLSServer(http.HandlerFunc(func(http.ResponseWriter, *http.Request) {}))
	client := testClient(server)
	server.Close()

	if _, err := client.Get(context.Background(), answeredRunID); err == nil {
		t.Fatal("a closed origin answered a run")
	}
}

// An answer this client cannot read is refused rather than half-decoded:
// past the limit, not the shape it promised, or with a second document
// hiding behind the first.
func TestAnAnswerThisClientCannotReadIsRefused(t *testing.T) {
	ctx := context.Background()

	flooding := sending(t, func(w http.ResponseWriter, _ *http.Request) {
		_, _ = w.Write([]byte(`{"id":"` + strings.Repeat("a", maxResponseBytes) + `"}`))
	})
	if _, err := flooding.Get(ctx, answeredRunID); err == nil ||
		!strings.Contains(err.Error(), "unreadable or exceeds limit") {
		t.Fatalf("a flooding answer gave %v", err)
	}

	strange := sending(t, func(w http.ResponseWriter, _ *http.Request) {
		_, _ = w.Write([]byte(`{"id":"` + answeredRunID + `","state":"queued","surprise":1}`))
	})
	if _, err := strange.Get(ctx, answeredRunID); err == nil ||
		!strings.Contains(err.Error(), "decode run API response") {
		t.Fatalf("an unknown field gave %v", err)
	}

	doubled := sending(t, func(w http.ResponseWriter, _ *http.Request) {
		_, _ = w.Write([]byte(`{"id":"` + answeredRunID + `","state":"queued"}{"id":"other"}`))
	})
	if _, err := doubled.Get(ctx, answeredRunID); err == nil ||
		!strings.Contains(err.Error(), "trailing JSON") {
		t.Fatalf("a doubled answer gave %v", err)
	}
}
