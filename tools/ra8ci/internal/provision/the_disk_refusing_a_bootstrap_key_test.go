// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package provision

import (
	"bytes"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
)

// keyFiles returns the reservation key files the store directory holds.
func keyFiles(t *testing.T, root string) []string {
	t.Helper()
	kept := make([]string, 0, 2)
	for _, name := range rootEntries(t, root) {
		if strings.HasSuffix(name, ".key") {
			kept = append(kept, name)
		}
	}
	return kept
}

// TestWriteAndCloseReportsAHandleThatTookNothing pins that the write helper
// answers on the write, the sync AND the close, so a key that never reached
// the disk is never published.
func TestWriteAndCloseReportsAHandleThatTookNothing(t *testing.T) {
	root := t.TempDir()

	spent, err := os.CreateTemp(root, "spent-*")
	if err != nil {
		t.Fatalf("create temporary file: %v", err)
	}
	if err := spent.Close(); err != nil {
		t.Fatalf("close temporary file: %v", err)
	}
	if err := writeAndClose(spent, []byte("private key bytes")); err == nil {
		t.Fatal("writeAndClose accepted a handle that was already closed")
	} else if err.Error() != "write reservation SSH private key" {
		t.Fatalf("write refusal reads %q", err.Error())
	}

	live, err := os.CreateTemp(root, "live-*")
	if err != nil {
		t.Fatalf("create temporary file: %v", err)
	}
	content := []byte("PRIVATE KEY\n")
	if err := writeAndClose(live, content); err != nil {
		t.Fatalf("writeAndClose refused a live handle: %v", err)
	}
	if err := live.Close(); err == nil {
		t.Fatal("writeAndClose left the handle open")
	}
	written, err := os.ReadFile(live.Name())
	if err != nil {
		t.Fatalf("read back written file: %v", err)
	}
	if !bytes.Equal(written, content) {
		t.Fatalf("file holds %q, wrote %q", written, content)
	}
}

// TestOneReservationKeepsOneKeyUnderConcurrentMints pins the losing side of
// the publish race: a caller that generated its own key and then lost the link
// hands back the key that won, never its own. Two callers holding different
// keys for one reservation is the failure this forbids, because only one of
// them is the key the guest was actually built with.
func TestOneReservationKeepsOneKeyUnderConcurrentMints(t *testing.T) {
	keys, root := reservationKeyStore(t)
	reservationID := reservationKeyID(t)

	const callers = 16
	var start sync.WaitGroup
	var done sync.WaitGroup
	start.Add(1)
	minted := make([]SSHAccessKey, callers)
	failures := make([]error, callers)
	for index := range callers {
		done.Add(1)
		go func(index int) {
			defer done.Done()
			start.Wait()
			minted[index], failures[index] = keys.Ensure(reservationID)
		}(index)
	}
	start.Done()
	done.Wait()

	for index, err := range failures {
		if err != nil {
			t.Fatalf("caller %d was refused: %v", index, err)
		}
	}
	for index, key := range minted {
		if key.PublicKey == "" {
			t.Fatalf("caller %d was handed no public key", index)
		}
		if key.PublicKey != minted[0].PublicKey {
			t.Fatalf("caller %d holds a different key for one reservation", index)
		}
		if key.PrivateKeyFile != filepath.Join(root, reservationID+".key") {
			t.Fatalf("caller %d points at %q", index, key.PrivateKeyFile)
		}
	}
	if held := keyFiles(t, root); len(held) != 1 {
		t.Fatalf("one reservation left %v", held)
	}
	if entries := rootEntries(t, root); len(entries) != 1 {
		t.Fatalf("mint race left %v behind", entries)
	}
}
