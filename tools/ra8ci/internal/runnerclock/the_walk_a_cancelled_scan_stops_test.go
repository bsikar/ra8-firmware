// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package runnerclock

import (
	"bytes"
	"context"
	"errors"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

// A scan of a thousand runs is minutes of requests, so the walk asks whether
// it is still wanted between runs rather than only at the socket. What is
// held here is that the answer is obeyed: a cancelled scan stops with the
// cancellation as its reason and reports nothing, instead of printing a
// verdict over the handful of runs it happened to reach.

// cancellingAfterTheRunList buffers each answer before handing it back, then
// cancels the scan once the run list has been delivered. Buffering is what
// makes the moment exact: the run list is already in memory, so the walk
// begins with a context that is already done rather than losing the page it
// was reading.
type cancellingAfterTheRunList struct {
	inner  http.RoundTripper
	cancel context.CancelFunc
}

func (c cancellingAfterTheRunList) RoundTrip(request *http.Request) (*http.Response, error) {
	response, err := c.inner.RoundTrip(request)
	if err != nil {
		return response, err
	}
	body, readErr := io.ReadAll(response.Body)
	_ = response.Body.Close()
	if readErr != nil {
		return nil, readErr
	}
	response.Body = io.NopCloser(bytes.NewReader(body))
	if !strings.HasSuffix(request.URL.Path, "/jobs") {
		c.cancel()
	}
	return response, nil
}

func TestACancelledScanStopsAtTheNextRunAndReportsNothing(t *testing.T) {
	served := 0
	server := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		served++
		if strings.HasSuffix(r.URL.Path, "/jobs") {
			_, _ = w.Write([]byte(`{"jobs":[]}`))
			return
		}
		_, _ = w.Write([]byte(oneRun))
	}))
	t.Cleanup(server.Close)

	ctx, cancel := context.WithCancel(context.Background())
	t.Cleanup(cancel)
	client := server.Client()
	client.Transport = cancellingAfterTheRunList{inner: client.Transport, cancel: cancel}
	api, err := newActionsAPIAt(server.URL, "token", client)
	if err != nil {
		t.Fatal(err)
	}

	var stdout bytes.Buffer
	code, scanErr := scan(ctx, api, "owner/repo", 1, nil, &stdout)
	if code != 2 {
		t.Fatalf("a cancelled scan answered %d, want 2", code)
	}
	if !errors.Is(scanErr, context.Canceled) {
		t.Fatalf("a cancelled scan should say so: %v", scanErr)
	}
	if stdout.String() != "" {
		t.Fatalf("a cancelled scan reported: %q", stdout.String())
	}
	if served != 1 {
		t.Fatalf("the walk spent %d requests after it was cancelled", served-1)
	}
}

func TestTheSameRunListIsStillWalkedWhenTheScanIsWanted(t *testing.T) {
	// The identical fixture, uncancelled. Without this the refusal above
	// could be the run list's doing rather than the cancellation's.
	_, _, err := scanning(t, oneRun, `{"jobs":[]}`, nil)
	if errors.Is(err, context.Canceled) {
		t.Fatalf("an uncancelled scan stopped as cancelled: %v", err)
	}
}
