// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package agent

import (
	"context"
	"errors"
	"net/http"
	"testing"
	"time"
)

// A heartbeat is the only thing holding an attempt open, so what it does when
// a beat fails decides whether the attempt is torn down. The distinction it
// has to keep is between a beat cut short by the attempt ending, which is
// expected and must stay quiet, and a beat the plane genuinely refused, which
// has to take the attempt down with it.

func TestABeatCutShortByTheAttemptEndingIsNotAFailure(t *testing.T) {
	arrived := make(chan struct{})
	release := make(chan struct{})
	agent, server := testAgent(t, func(w http.ResponseWriter, _ *http.Request) {
		select {
		case arrived <- struct{}{}:
		default:
		}
		<-release
	})
	defer server.Close()
	defer close(release)
	agent.beat = time.Millisecond
	ctx, cancel := context.WithCancel(context.Background())
	answered := make(chan error, 1)
	go func() { answered <- agent.heartbeat(ctx, testAssignment(), cancel) }()
	select {
	case <-arrived:
	case <-time.After(5 * time.Second):
		cancel()
		t.Fatal("no beat reached the plane")
	}
	// The task deadline or the local completion path cancels the attempt
	// while a beat is in flight. Reporting that as a heartbeat failure would
	// suppress the terminal evidence upload, which is the one thing that
	// still has to happen after the attempt ends.
	cancel()
	select {
	case err := <-answered:
		if err != nil {
			t.Fatalf("a beat cut short by the attempt ending answered %v", err)
		}
	case <-time.After(5 * time.Second):
		t.Fatal("heartbeat did not return after the attempt was cancelled")
	}
}

func TestABeatThePlaneRefusesTakesTheAttemptDown(t *testing.T) {
	agent, server := testAgent(t, func(w http.ResponseWriter, _ *http.Request) {
		w.WriteHeader(http.StatusInternalServerError)
	})
	defer server.Close()
	agent.beat = time.Millisecond
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	torn := false
	err := agent.heartbeat(ctx, testAssignment(), func() { torn = true; cancel() })
	if !errors.Is(err, ErrServerProtocol) {
		t.Fatalf("a refused beat answered %v", err)
	}
	// The refusal alone is not the point: the attempt has to come down with
	// it, because a host still running a task the plane has stopped hearing
	// from is exactly what the fence exists to prevent.
	if !torn {
		t.Fatal("a refused beat left the attempt running")
	}
	if ctx.Err() == nil {
		t.Fatal("the attempt context outlived the refused beat")
	}
}

func TestAChunkThatWillNotValidateIsNeverOfferedAndClosesTheDoor(t *testing.T) {
	agent, claims := countingPlane(t, func(w http.ResponseWriter, _ *http.Request) {
		w.WriteHeader(http.StatusOK)
	})
	unusable := testAssignment()
	unusable.AttemptID = ""
	uploader := &logUploader{agent: agent, ctx: context.Background(), assignment: unusable}
	written, err := uploader.write("format-check", "stdout", []byte("a line the plane will never see\n"))
	if err == nil {
		t.Fatal("a chunk that does not validate was offered")
	}
	if written != 0 {
		t.Fatalf("wrote %d bytes of an unusable chunk", written)
	}
	if got := claims.Load(); got != 0 {
		t.Fatalf("an unusable chunk spent %d calls", got)
	}
	// Nothing is held for a later flush either: a chunk refused here was
	// never a chunk, so there is nothing the grace period could recover.
	if uploader.pending != nil {
		t.Fatal("an unusable chunk was held for the flush")
	}
	sequence, stored := uploader.status()
	if sequence != 0 || !errors.Is(stored, err) {
		t.Fatalf("status = %d, %v", sequence, stored)
	}
	// And the door stays shut: the next write is turned away without being
	// offered, and its bytes are counted as gone rather than pending.
	second := []byte("and neither will this one\n")
	if _, err := uploader.write("format-check", "stdout", second); err == nil {
		t.Fatal("a later write got past a closed door")
	}
	if uploader.dropped != int64(len(second)) {
		t.Fatalf("dropped = %d, want %d", uploader.dropped, len(second))
	}
	if got := claims.Load(); got != 0 {
		t.Fatalf("a closed uploader spent %d calls", got)
	}
}
