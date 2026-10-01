// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"context"
	"errors"
	"strings"
	"testing"

	"github.com/actions/scaleset"
)

// A JIT credential is single use, so a mint that failed has to be reported
// as a failure and nothing else. Reporting an empty config with no error
// would hand the caller a credential-shaped zero value it could deliver to
// a machine that would then sit waiting for a job it can never accept.

// A forge that refuses the mint is reported as the mint it was, with no
// credential handed back and nothing left for the caller to clear.
func TestAMintTheForgeRefusedIsNotACredential(t *testing.T) {
	admin := &scriptedAdmin{}
	session := &Session{admin: admin, scaleSetID: 42}

	config, err := session.GenerateJIT(context.Background(), "ra8ci-4429117744-2")
	if err == nil {
		t.Fatal("a refused mint answered a credential")
	}
	if !strings.Contains(err.Error(), "generate GitHub JIT runner config") ||
		!strings.Contains(err.Error(), "unexpected JIT request") {
		t.Fatalf("the failure lost its subject or its reason: %v", err)
	}
	if config.Runner.ID != 0 || config.Runner.Name != "" || len(config.EncodedConfig) != 0 {
		t.Fatalf("a refused mint answered %+v", config.Runner)
	}
	if admin.jitRequests != 1 {
		t.Fatalf("the forge was asked %d times for one mint", admin.jitRequests)
	}

	// Clearing what a refused mint handed back is safe, which is what lets
	// a caller defer the clear the moment it calls.
	config.Clear()
}

// A session missing any part of its identity never reaches the forge. A
// name this plane would not have minted is refused here too: GenerateJIT is
// how a name becomes real, so an unmintable one must not get that far.
func TestAnUnmintableRequestNeverReachesTheForge(t *testing.T) {
	admin := &scriptedAdmin{}
	ctx := context.Background()

	var missing *Session
	if _, err := missing.GenerateJIT(ctx, "ra8ci-4429117744-2"); err == nil {
		t.Fatal("a session that was never built minted a credential")
	}
	if _, err := (&Session{scaleSetID: 42}).GenerateJIT(ctx, "ra8ci-4429117744-2"); err == nil {
		t.Fatal("a session with no forge minted a credential")
	}
	if _, err := (&Session{admin: admin}).GenerateJIT(ctx, "ra8ci-4429117744-2"); err == nil {
		t.Fatal("a session with no scale set minted a credential")
	}

	session := &Session{admin: admin, scaleSetID: 42}
	var none context.Context
	if _, err := session.GenerateJIT(none, "ra8ci-4429117744-2"); err == nil {
		t.Fatal("a mint with no context reached the forge")
	}
	for _, name := range []string{
		"", "-leading-dash", "ra8ci_4429117744_2", "ra8ci 4429117744", "../etc/passwd",
		"ra8ci-4429117744-2/extra", strings.Repeat("r", 65), "ra8ci-4429117744-2\n",
	} {
		if _, err := session.GenerateJIT(ctx, name); err == nil {
			t.Fatalf("name %q was minted", name)
		}
	}
	if admin.jitRequests != 0 {
		t.Fatalf("the forge was asked %d times for a request it should never have seen", admin.jitRequests)
	}
}

// A removal the forge refuses is reported with the runner named, and a
// runner the forge does not hold is not an error: a resumed revocation sees
// exactly that on its second pass.
func TestARemovalTheForgeRefusedNamesTheRunner(t *testing.T) {
	held := &scaleset.RunnerReference{ID: 41, Name: "ra8ci-4429117744-2", RunnerScaleSetID: 42}
	refusing := &scriptedAdmin{runner: held, removeErr: errors.New("forge unavailable")}
	session := &Session{admin: refusing, scaleSetID: 42}

	err := session.RemoveRunner(context.Background(), 41)
	if err == nil || !strings.Contains(err.Error(), "remove GitHub runner 41") ||
		!strings.Contains(err.Error(), "forge unavailable") {
		t.Fatalf("a refused removal answered %v", err)
	}
	if len(refusing.removals) != 1 || refusing.removals[0] != 41 {
		t.Fatalf("the forge saw removals %v", refusing.removals)
	}

	absent := &scriptedAdmin{}
	if err := (&Session{admin: absent, scaleSetID: 42}).RemoveRunner(context.Background(), 41); err != nil {
		t.Fatalf("a runner the forge does not hold answered %v", err)
	}
	if len(absent.removals) != 0 {
		t.Fatalf("an absent runner was still removed: %v", absent.removals)
	}

	// A lookup that fails stops the removal rather than removing blind.
	unreachable := &scriptedAdmin{runnerErr: errors.New("forge unreachable")}
	if err := (&Session{admin: unreachable, scaleSetID: 42}).RemoveRunner(context.Background(), 41); err == nil {
		t.Fatal("a removal went ahead over a failed lookup")
	}
	if len(unreachable.removals) != 0 {
		t.Fatalf("a blind removal reached the forge: %v", unreachable.removals)
	}
}
