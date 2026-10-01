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

// sealedRoot makes the store directory unwritable for the rest of the test and
// restores it before the temporary directory is removed.
func sealedRoot(t *testing.T, root string) {
	t.Helper()
	if err := os.Chmod(root, 0o500); err != nil {
		t.Fatalf("seal store directory: %v", err)
	}
	t.Cleanup(func() { _ = os.Chmod(root, 0o700) })
}

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

// TestADirectoryThatWillNotTakeANewKey pins the refusal a mint meets when the
// service disk has gone read-only under it. The reservation is left with no
// key at all, which is the honest outcome: a half-written key would be worse
// than none, because a guest booted against it can never be reached again.
func TestADirectoryThatWillNotTakeANewKey(t *testing.T) {
	keys, root := reservationKeyStore(t)
	reservationID := reservationKeyID(t)
	sealedRoot(t, root)

	key, err := keys.Ensure(reservationID)
	if err == nil {
		t.Fatal("Ensure minted a key into a directory it cannot write")
	}
	if err.Error() != "create temporary reservation SSH key" {
		t.Fatalf("mint refusal reads %q", err.Error())
	}
	if key.PublicKey != "" || key.PrivateKeyFile != "" {
		t.Fatalf("refused mint still described a key: %+v", key)
	}
	if entries := rootEntries(t, root); len(entries) != 0 {
		t.Fatalf("refused mint left %v behind", entries)
	}
}

// TestAKeyThatCannotBeUnlinked pins that Remove reports the failure rather
// than reporting success over a key that is still on disk. A caller that
// believes a key is gone stops guarding it.
func TestAKeyThatCannotBeUnlinked(t *testing.T) {
	keys, root := reservationKeyStore(t)
	reservationID := reservationKeyID(t)
	file := planted(t, root, reservationID, 0o600, []byte("not a key, never read\n"))
	sealedRoot(t, root)

	err := keys.Remove(reservationID)
	if err == nil {
		t.Fatal("Remove reported success over a key it could not unlink")
	}
	if err.Error() != "remove reservation SSH private key" {
		t.Fatalf("remove refusal reads %q", err.Error())
	}
	if _, statErr := os.Lstat(file); statErr != nil {
		t.Fatalf("key file went missing after a refused remove: %v", statErr)
	}
}

// TestAKeyThePolicyAdmitsButTheDiskWillNotOpen pins the gap between the
// private-file policy and the read itself. A mode 0o000 file satisfies every
// policy check (regular, in range, no group or other bits) and still cannot be
// opened, so the refusal has to come from the open and not from the policy.
func TestAKeyThePolicyAdmitsButTheDiskWillNotOpen(t *testing.T) {
	keys, root := reservationKeyStore(t)
	reservationID := reservationKeyID(t)
	file := planted(t, root, reservationID, 0o000, []byte("sealed\n"))

	if _, err := keys.Load(reservationID); err == nil {
		t.Fatal("Load read a key file it has no permission to open")
	} else if err.Error() != "read reservation SSH private key" {
		t.Fatalf("load refusal reads %q", err.Error())
	}

	// The same file must stop Ensure too, and stop it BEFORE it mints:
	// an unreadable key is a key that may still be installed on a live
	// guest, so quietly replacing it would lock us out of that guest.
	if _, err := keys.Ensure(reservationID); err == nil {
		t.Fatal("Ensure minted over a key it could not read")
	} else if err.Error() != "read reservation SSH private key" {
		t.Fatalf("ensure refusal reads %q", err.Error())
	}
	if entries := rootEntries(t, root); len(entries) != 1 || entries[0] != filepath.Base(file) {
		t.Fatalf("store directory holds %v after two refusals", entries)
	}
	info, err := os.Lstat(file)
	if err != nil {
		t.Fatalf("stat sealed key: %v", err)
	}
	if info.Mode().Perm() != 0o000 || info.Size() != int64(len("sealed\n")) {
		t.Fatalf("sealed key changed under a refusal: mode %v size %d",
			info.Mode().Perm(), info.Size())
	}
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
