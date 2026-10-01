//go:build integration

package store

import (
	"context"
	"encoding/json"
	"errors"
	"strings"
	"testing"
	"time"
)

// The parent a child run is allowed to name.
//
// A run may be filed as the child of another, and the link is what later
// reads walk to put a retry next to the work it retried. Nothing pinned
// what that link is checked against, so the whole parent block of CreateRun
// was unexercised: an absent parent, and a parent belonging to a different
// repository, which is the one that matters, because a child filed under
// another project's run quietly carries that project's history forward.

func childlessRun(t *testing.T, repository string) CreateRunInput {
	t.Helper()
	return CreateRunInput{Trigger: "integration", ActorID: "integration-operator",
		Repository: repository, CommitSHA: strings.Repeat("a", 40),
		SnapshotSHA256: strings.Repeat("b", 64), CatalogSHA256: strings.Repeat("c", 64),
		IdempotencyKey: "parent-" + mustID(t), RequestSHA256: strings.Repeat("d", 64),
		Tasks: []TaskInput{{Key: "one", Name: "format-check",
			Arguments: json.RawMessage(`{"argv":[]}`), Tier: "required",
			Scope: "safe-local-read-only", HostClass: "linux-vm", DeadlineSeconds: 60}}}
}

func TestIntegrationAChildRunIsFiledUnderTheParentItNames(t *testing.T) {
	st, _ := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
	defer cancel()

	parent, err := st.CreateRun(ctx, childlessRun(t, "bsikar/ra8-firmware"))
	if err != nil {
		t.Fatalf("creating the parent run: %v", err)
	}

	childInput := childlessRun(t, "bsikar/ra8-firmware")
	childInput.ParentRunID = parent.ID
	child, err := st.CreateRun(ctx, childInput)
	if err != nil {
		t.Fatalf("creating a child run: %v", err)
	}
	if child.ParentRunID != parent.ID {
		t.Fatalf("child filed under parent %q, want %q", child.ParentRunID, parent.ID)
	}
	// The link has to survive the write, not just the return value: a later
	// read is what actually walks it.
	reread, err := st.GetRun(ctx, child.ID)
	if err != nil || reread.ParentRunID != parent.ID {
		t.Fatalf("re-read child names parent %q: %v", reread.ParentRunID, err)
	}
	// A run with no parent is not filed under one, so a reader can tell the
	// difference between a root run and a child.
	if parent.ParentRunID != "" {
		t.Fatalf("a root run came back with parent %q", parent.ParentRunID)
	}
}

func TestIntegrationAChildRunRefusesAParentItCannotBeFiledUnder(t *testing.T) {
	st, _ := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
	defer cancel()

	t.Run("a parent that does not exist", func(t *testing.T) {
		in := childlessRun(t, "bsikar/ra8-firmware")
		in.ParentRunID = mustID(t)
		// Reported as missing rather than invalid: the identifier is
		// well-formed, there is simply no such run.
		if _, err := st.CreateRun(ctx, in); !errors.Is(err, ErrNotFound) {
			t.Fatalf("an absent parent answered %v, want not found", err)
		}
	})

	t.Run("a parent in another repository", func(t *testing.T) {
		elsewhere, err := st.CreateRun(ctx, childlessRun(t, "bsikar/some-other-project"))
		if err != nil {
			t.Fatalf("creating the foreign parent: %v", err)
		}
		in := childlessRun(t, "bsikar/ra8-firmware")
		in.ParentRunID = elsewhere.ID
		// This is the refusal with teeth. A child filed across repositories
		// carries the other project's lineage into every read that walks
		// the link, and nothing downstream would question it.
		if _, err := st.CreateRun(ctx, in); !errors.Is(err, ErrInvalid) {
			t.Fatalf("a parent in another repository answered %v, want invalid", err)
		}
	})

	t.Run("a parent identifier that is not one", func(t *testing.T) {
		in := childlessRun(t, "bsikar/ra8-firmware")
		in.ParentRunID = "not-an-id"
		// Refused by validateRun before a transaction opens at all.
		if _, err := st.CreateRun(ctx, in); !errors.Is(err, ErrInvalid) {
			t.Fatalf("a malformed parent answered %v, want invalid", err)
		}
	})
}
