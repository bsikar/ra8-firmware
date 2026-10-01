// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package provision

import (
	"encoding/base64"
	"encoding/binary"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

func TestSSHAccessStorePersistsOnePrivateKeyPerReservation(t *testing.T) {
	root := filepath.Join(t.TempDir(), "keys")
	keys, err := NewSSHAccessStore(root)
	if err != nil {
		t.Fatal(err)
	}
	reservationID, err := store.NewID()
	if err != nil {
		t.Fatal(err)
	}
	const callers = 16
	results := make(chan SSHAccessKey, callers)
	errorsFound := make(chan error, callers)
	var workers sync.WaitGroup
	for range callers {
		workers.Add(1)
		go func() {
			defer workers.Done()
			key, err := keys.Ensure(reservationID)
			if err != nil {
				errorsFound <- err
				return
			}
			results <- key
		}()
	}
	workers.Wait()
	close(results)
	close(errorsFound)
	for err := range errorsFound {
		t.Fatal(err)
	}
	var first SSHAccessKey
	for key := range results {
		if first.PublicKey == "" {
			first = key
			continue
		}
		if key.PublicKey != first.PublicKey || key.PrivateKeyFile != first.PrivateKeyFile {
			t.Fatal("concurrent Ensure returned different reservation keys")
		}
	}
	if first.PublicKey == "" || !strings.HasPrefix(first.PublicKey, "ssh-ed25519 ") {
		t.Fatalf("invalid OpenSSH public key: %q", first.PublicKey)
	}
	fields := strings.Fields(first.PublicKey)
	if len(fields) != 3 {
		t.Fatalf("public key has unexpected fields: %q", first.PublicKey)
	}
	wire, err := base64.StdEncoding.DecodeString(fields[1])
	if err != nil || len(wire) < 4 {
		t.Fatal("public key wire encoding is invalid")
	}
	typeLength := int(binary.BigEndian.Uint32(wire[:4]))
	if typeLength != len("ssh-ed25519") || string(wire[4:4+typeLength]) != "ssh-ed25519" {
		t.Fatal("public key algorithm does not match its OpenSSH label")
	}
	info, err := os.Stat(first.PrivateKeyFile)
	loaded, err := keys.Load(reservationID)
	if err != nil || loaded.PublicKey != first.PublicKey {
		t.Fatalf("Load changed or failed the reservation key: %+v, %v", loaded, err)
	}
	if err != nil || info.Mode().Perm() != 0o600 {
		t.Fatalf("private key mode is not 0600: %v, %v", info, err)
	}
	again, err := keys.Ensure(reservationID)
	if err != nil || again.PublicKey != first.PublicKey {
		if _, err := keys.Load(reservationID); err == nil {
			t.Fatal("Load created or returned a removed key")
		}
		t.Fatalf("key did not survive reopening: %+v, %v", again, err)
	}
	if err := keys.Remove(reservationID); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Lstat(first.PrivateKeyFile); !os.IsNotExist(err) {
		t.Fatalf("private key remains after removal: %v", err)
	}
	recreated, err := keys.Ensure(reservationID)
	if err != nil || recreated.PublicKey == first.PublicKey {
		t.Fatalf("removed key was not replaced: %+v, %v", recreated, err)
	}
}

func TestSSHAccessStoreRejectsUnsafeKeyPermissions(t *testing.T) {
	keys, err := NewSSHAccessStore(filepath.Join(t.TempDir(), "keys"))
	if err != nil {
		t.Fatal(err)
	}
	reservationID, err := store.NewID()
	if err != nil {
		t.Fatal(err)
	}
	key, err := keys.Ensure(reservationID)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.Chmod(key.PrivateKeyFile, 0o644); err != nil {
		t.Fatal(err)
	}
	if _, err := keys.Ensure(reservationID); err == nil {
		t.Fatal("accepted a group-readable SSH private key")
	}
	if err := keys.Remove(reservationID); err == nil {
		t.Fatal("removed a key that violates private-file policy")
	}
}
