// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package provision

import (
	"bytes"
	"crypto/ecdsa"
	"crypto/ed25519"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/x509"
	"encoding/base64"
	"encoding/binary"
	"encoding/pem"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// reservationKeyStore opens a store over a fresh private directory.
func reservationKeyStore(t *testing.T) (*SSHAccessStore, string) {
	t.Helper()
	root := filepath.Join(t.TempDir(), "keys")
	keys, err := NewSSHAccessStore(root)
	if err != nil {
		t.Fatalf("open SSH access-key store: %v", err)
	}
	return keys, root
}

// reservationKeyID returns an identifier the store will accept.
func reservationKeyID(t *testing.T) string {
	t.Helper()
	id, err := store.NewID()
	if err != nil {
		t.Fatalf("new reservation ID: %v", err)
	}
	return id
}

// planted writes content to one reservation's key file with the given mode.
func planted(t *testing.T, root, reservationID string, mode os.FileMode, content []byte) string {
	t.Helper()
	file := filepath.Join(root, reservationID+".key")
	if err := os.WriteFile(file, content, mode); err != nil {
		t.Fatalf("plant key file: %v", err)
	}
	if err := os.Chmod(file, mode); err != nil {
		t.Fatalf("plant key mode: %v", err)
	}
	return file
}

// rootEntries lists what the store directory holds right now.
func rootEntries(t *testing.T, root string) []string {
	t.Helper()
	listed, err := os.ReadDir(root)
	if err != nil {
		t.Fatalf("read store directory: %v", err)
	}
	names := make([]string, 0, len(listed))
	for _, entry := range listed {
		names = append(names, entry.Name())
	}
	return names
}

// TestTheDirectoryAStoreWillOpen pins the last cheap refusal: past it every
// reservation writes a private key into whatever directory was accepted.
func TestTheDirectoryAStoreWillOpen(t *testing.T) {
	if _, err := NewSSHAccessStore("relative/keys"); err == nil ||
		!strings.Contains(err.Error(), "absolute") {
		t.Fatalf("relative directory admitted or misreported: %v", err)
	}
	if _, err := NewSSHAccessStore(""); err == nil {
		t.Fatal("empty directory admitted")
	}

	base := t.TempDir()
	occupied := filepath.Join(base, "file")
	if err := os.WriteFile(occupied, []byte("x"), 0o600); err != nil {
		t.Fatal(err)
	}
	if _, err := NewSSHAccessStore(filepath.Join(occupied, "keys")); err == nil ||
		!strings.Contains(err.Error(), "prepare SSH access-key directory") {
		t.Fatalf("directory under a regular file admitted or misreported: %v", err)
	}

	real := filepath.Join(base, "real")
	if err := os.Mkdir(real, 0o700); err != nil {
		t.Fatal(err)
	}
	link := filepath.Join(base, "link")
	symlinkTest(t, real, link)
	if _, err := NewSSHAccessStore(link); err == nil ||
		!strings.Contains(err.Error(), "not a real directory") {
		t.Fatalf("symlinked directory admitted or misreported: %v", err)
	}
}

// TestEveryDoorRefusesAnIdentifierTheStoreNeverIssued pins that the identifier
// guard runs on all three doors, so no caller reaches the filesystem with a
// name nobody minted.
func TestEveryDoorRefusesAnIdentifierTheStoreNeverIssued(t *testing.T) {
	keys, root := reservationKeyStore(t)
	valid := reservationKeyID(t)
	mutate := func(index int, character byte) string {
		raw := []byte(valid)
		raw[index] = character
		return string(raw)
	}
	refused := map[string]string{
		"empty":                  "",
		"not an identifier":      "not-a-uuid",
		"one character short":    valid[:35],
		"one character long":     valid + "0",
		"uppercase hex":          strings.ToUpper(valid),
		"another UUID version":   mutate(14, '4'),
		"a variant outside 89ab": mutate(19, 'c'),
		"a non-hex digit":        mutate(0, 'g'),
		"a moved separator":      mutate(8, 'a'),
		"a traversal prefix":     "../" + valid,
		"the file name itself":   valid + ".key",
		"an underscore group":    strings.Replace(valid, "-", "_", 1),
	}
	for name, reservationID := range refused {
		if _, err := keys.Ensure(reservationID); err == nil ||
			err.Error() != "invalid SSH access-key reservation" {
			t.Fatalf("Ensure admitted or misreported %s (%q): %v", name, reservationID, err)
		}
		if _, err := keys.Load(reservationID); err == nil ||
			err.Error() != "invalid SSH access-key reservation" {
			t.Fatalf("Load admitted or misreported %s (%q): %v", name, reservationID, err)
		}
		if err := keys.Remove(reservationID); err == nil ||
			err.Error() != "invalid SSH access-key reservation" {
			t.Fatalf("Remove admitted or misreported %s (%q): %v", name, reservationID, err)
		}
	}
	if names := rootEntries(t, root); len(names) != 0 {
		t.Fatalf("a refused identifier still touched the directory: %v", names)
	}

	var absent *SSHAccessStore
	if _, err := absent.Ensure(valid); err == nil {
		t.Fatal("a nil store minted a key")
	}
	if _, err := absent.Load(valid); err == nil {
		t.Fatal("a nil store loaded a key")
	}
	if err := absent.Remove(valid); err == nil {
		t.Fatal("a nil store reported a removal")
	}
}

// TestLoadNeverMintsAndEnsureMintsOnce pins the difference between the two
// reads: only one of them may bring a key into existence.
func TestLoadNeverMintsAndEnsureMintsOnce(t *testing.T) {
	keys, root := reservationKeyStore(t)
	first := reservationKeyID(t)
	second := reservationKeyID(t)

	if _, err := keys.Load(first); err == nil {
		t.Fatal("Load answered for a reservation that has no key")
	}
	if names := rootEntries(t, root); len(names) != 0 {
		t.Fatalf("Load created something: %v", names)
	}

	minted, err := keys.Ensure(first)
	if err != nil {
		t.Fatal(err)
	}
	if names := rootEntries(t, root); len(names) != 1 || names[0] != first+".key" {
		t.Fatalf("Ensure left something besides the key behind: %v", names)
	}
	loaded, err := keys.Load(first)
	if err != nil || loaded != minted {
		t.Fatalf("Load returned a different key: %+v vs %+v, %v", loaded, minted, err)
	}

	other, err := keys.Ensure(second)
	if err != nil {
		t.Fatal(err)
	}
	if other.PublicKey == minted.PublicKey || other.PrivateKeyFile == minted.PrivateKeyFile {
		t.Fatal("two reservations were handed the same key")
	}
	if names := rootEntries(t, root); len(names) != 2 {
		t.Fatalf("two reservations did not leave two keys: %v", names)
	}

	reopened, err := NewSSHAccessStore(root)
	if err != nil {
		t.Fatal(err)
	}
	survived, err := reopened.Ensure(first)
	if err != nil || survived != minted {
		t.Fatalf("a new store over the same directory rotated the key: %+v, %v", survived, err)
	}
}

// TestTheAdvertisedPublicKeyIsTheStoredPrivateKeysOwn pins that the line handed
// to Terraform authorises exactly the private key kept on disk. A drift here
// boots a guest nobody holds the key to.
func TestTheAdvertisedPublicKeyIsTheStoredPrivateKeysOwn(t *testing.T) {
	keys, _ := reservationKeyStore(t)
	reservationID := reservationKeyID(t)
	key, err := keys.Ensure(reservationID)
	if err != nil {
		t.Fatal(err)
	}

	fields := strings.Fields(key.PublicKey)
	if len(fields) != 3 || fields[0] != "ssh-ed25519" {
		t.Fatalf("public key is not an OpenSSH ed25519 line: %q", key.PublicKey)
	}
	if fields[2] != "ra8ci-"+reservationID {
		t.Fatalf("public key comment does not name the reservation: %q", fields[2])
	}
	wire, err := base64.StdEncoding.DecodeString(fields[1])
	if err != nil {
		t.Fatalf("public key blob is not base64: %v", err)
	}
	const typeName = "ssh-ed25519"
	if len(wire) != 4+len(typeName)+4+ed25519.PublicKeySize {
		t.Fatalf("public key blob has unexpected length %d", len(wire))
	}
	if int(binary.BigEndian.Uint32(wire[:4])) != len(typeName) ||
		string(wire[4:4+len(typeName)]) != typeName {
		t.Fatal("public key blob does not name ssh-ed25519")
	}
	offset := 4 + len(typeName)
	if int(binary.BigEndian.Uint32(wire[offset:offset+4])) != ed25519.PublicKeySize {
		t.Fatal("public key blob does not carry a 32-byte key")
	}
	advertised := wire[offset+4:]

	stored, err := os.ReadFile(key.PrivateKeyFile)
	if err != nil {
		t.Fatal(err)
	}
	block, rest := pem.Decode(stored)
	if block == nil || len(rest) != 0 || block.Type != "PRIVATE KEY" {
		t.Fatal("stored key is not a single PRIVATE KEY block")
	}
	parsed, err := x509.ParsePKCS8PrivateKey(block.Bytes)
	if err != nil {
		t.Fatalf("stored key is not PKCS#8: %v", err)
	}
	private, ok := parsed.(ed25519.PrivateKey)
	if !ok {
		t.Fatalf("stored key is not ed25519: %T", parsed)
	}
	if !bytes.Equal(advertised, private.Public().(ed25519.PublicKey)) {
		t.Fatal("the advertised public key is not the stored private key's own")
	}
}

// TestAKeyFileTheStoreWillNotRead pins every refusal a key file meets, and the
// reason each is given, because the reason is what tells an operator whether
// the file is unsafe or simply not a key.
func TestAKeyFileTheStoreWillNotRead(t *testing.T) {
	keys, root := reservationKeyStore(t)
	good, err := keys.Ensure(reservationKeyID(t))
	if err != nil {
		t.Fatal(err)
	}
	valid, err := os.ReadFile(good.PrivateKeyFile)
	if err != nil {
		t.Fatal(err)
	}

	unsafe := []struct {
		name    string
		content []byte
		mode    os.FileMode
	}{
		{"empty", nil, 0o600},
		{"group readable", valid, 0o640},
		{"world readable", valid, 0o604},
		{"executable by others", valid, 0o601},
		{"over the size bound", bytes.Repeat([]byte("k"), maxSSHPrivateKeyBytes+1), 0o600},
	}
	for _, shape := range unsafe {
		reservationID := reservationKeyID(t)
		planted(t, root, reservationID, shape.mode, shape.content)
		if _, err := keys.Load(reservationID); err == nil ||
			err.Error() != "reservation SSH key violates private-file policy" {
			t.Fatalf("%s admitted or misreported: %v", shape.name, err)
		}
	}

	malformed := []struct {
		name    string
		content []byte
		reason  string
	}{
		{"not PEM at all", bytes.Repeat([]byte("k"), maxSSHPrivateKeyBytes),
			"reservation SSH private key is malformed"},
		{"trailing bytes after the block", append(append([]byte{}, valid...), 'x'),
			"reservation SSH private key is malformed"},
		{"another PEM type", pem.EncodeToMemory(&pem.Block{
			Type: "OPENSSH PRIVATE KEY", Bytes: []byte("body")}),
			"reservation SSH private key is malformed"},
		{"a PRIVATE KEY block of junk", pem.EncodeToMemory(&pem.Block{
			Type: "PRIVATE KEY", Bytes: []byte("not DER")}),
			"parse reservation SSH private key"},
	}
	for _, shape := range malformed {
		reservationID := reservationKeyID(t)
		planted(t, root, reservationID, 0o600, shape.content)
		if _, err := keys.Load(reservationID); err == nil || err.Error() != shape.reason {
			t.Fatalf("%s admitted or misreported: %v", shape.name, err)
		}
	}

	elliptical, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	der, err := x509.MarshalPKCS8PrivateKey(elliptical)
	if err != nil {
		t.Fatal(err)
	}
	wrongType := reservationKeyID(t)
	planted(t, root, wrongType, 0o600,
		pem.EncodeToMemory(&pem.Block{Type: "PRIVATE KEY", Bytes: der}))
	if _, err := keys.Load(wrongType); err == nil ||
		err.Error() != "reservation SSH key has unexpected type" {
		t.Fatalf("a well-formed non-ed25519 key admitted or misreported: %v", err)
	}
}

// TestASizeBoundThatAdmitsItsOwnLimit pins that the bound is inclusive: the
// file at exactly the limit is refused for what it CONTAINS, not for its size.
func TestASizeBoundThatAdmitsItsOwnLimit(t *testing.T) {
	keys, root := reservationKeyStore(t)
	atLimit := reservationKeyID(t)
	planted(t, root, atLimit, 0o600, bytes.Repeat([]byte("k"), maxSSHPrivateKeyBytes))
	if _, err := keys.Load(atLimit); err == nil ||
		err.Error() != "reservation SSH private key is malformed" {
		t.Fatalf("a file at exactly the bound was judged on its size: %v", err)
	}
	past := reservationKeyID(t)
	planted(t, root, past, 0o600, bytes.Repeat([]byte("k"), maxSSHPrivateKeyBytes+1))
	if _, err := keys.Load(past); err == nil ||
		err.Error() != "reservation SSH key violates private-file policy" {
		t.Fatalf("a file one byte past the bound was read anyway: %v", err)
	}
}

// TestAKeyPathThatIsNotAFile pins that neither a directory nor a symlink
// standing where a key belongs is followed, on every door.
func TestAKeyPathThatIsNotAFile(t *testing.T) {
	keys, root := reservationKeyStore(t)
	real, err := keys.Ensure(reservationKeyID(t))
	if err != nil {
		t.Fatal(err)
	}

	pointing := reservationKeyID(t)
	pointer := filepath.Join(root, pointing+".key")
	symlinkTest(t, real.PrivateKeyFile, pointer)
	if _, err := keys.Load(pointing); err == nil ||
		err.Error() != "reservation SSH key violates private-file policy" {
		t.Fatalf("a symlinked key was followed or misreported: %v", err)
	}
	if _, err := keys.Ensure(pointing); err == nil {
		t.Fatal("Ensure wrote through a symlink standing where a key belongs")
	}
	if err := keys.Remove(pointing); err == nil ||
		err.Error() != "refusing to remove an unsafe SSH key file" {
		t.Fatalf("a symlinked key was removed or misreported: %v", err)
	}
	if _, err := os.Lstat(real.PrivateKeyFile); err != nil {
		t.Fatalf("the key the symlink pointed at was disturbed: %v", err)
	}

	standing := reservationKeyID(t)
	if err := os.Mkdir(filepath.Join(root, standing+".key"), 0o700); err != nil {
		t.Fatal(err)
	}
	if _, err := keys.Load(standing); err == nil ||
		err.Error() != "reservation SSH key violates private-file policy" {
		t.Fatalf("a directory was read as a key: %v", err)
	}
	if err := keys.Remove(standing); err == nil ||
		err.Error() != "refusing to remove an unsafe SSH key file" {
		t.Fatalf("a directory was removed as a key: %v", err)
	}
}

// TestRemoveIsForgivingAboutAbsenceAndExactAboutPresence pins that teardown is
// safe to repeat and never reaches past the reservation it was given.
func TestRemoveIsForgivingAboutAbsenceAndExactAboutPresence(t *testing.T) {
	keys, _ := reservationKeyStore(t)
	absent := reservationKeyID(t)
	if err := keys.Remove(absent); err != nil {
		t.Fatalf("removing an absent key reported an error: %v", err)
	}
	if err := keys.Remove(absent); err != nil {
		t.Fatalf("removing an absent key twice reported an error: %v", err)
	}

	going := reservationKeyID(t)
	staying := reservationKeyID(t)
	doomed, err := keys.Ensure(going)
	if err != nil {
		t.Fatal(err)
	}
	kept, err := keys.Ensure(staying)
	if err != nil {
		t.Fatal(err)
	}
	if err := keys.Remove(going); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Lstat(doomed.PrivateKeyFile); !os.IsNotExist(err) {
		t.Fatalf("the named key survived removal: %v", err)
	}
	survivor, err := keys.Load(staying)
	if err != nil || survivor != kept {
		t.Fatalf("removal reached the neighbouring reservation: %+v, %v", survivor, err)
	}
	if _, err := keys.Load(going); err == nil {
		t.Fatal("Load answered for a removed key")
	}
	if err := keys.Remove(going); err != nil {
		t.Fatalf("removing an already removed key reported an error: %v", err)
	}
}
