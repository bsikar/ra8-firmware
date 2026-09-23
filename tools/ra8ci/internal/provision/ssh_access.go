// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

// Package provision contains the trusted Terraform/Ansible boundary used by
// the control plane. It never accepts backend configuration from a job.
package provision

import (
	"crypto/ed25519"
	"crypto/rand"
	"crypto/x509"
	"encoding/base64"
	"encoding/binary"
	"encoding/pem"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

const maxSSHPrivateKeyBytes = 8192

// SSHAccessKey exposes only the public key and private-key path. Private key
// bytes remain in a reservation-specific owner-only file.
type SSHAccessKey struct {
	PublicKey      string
	PrivateKeyFile string
}

// SSHAccessStore keeps per-reservation bootstrap keys on the protected ra8ci
// service disk so process restarts do not make an active VM unreachable.
type SSHAccessStore struct {
	root string
}

// NewSSHAccessStore requires an absolute private service-data directory.
func NewSSHAccessStore(root string) (*SSHAccessStore, error) {
	if !filepath.IsAbs(root) {
		return nil, errors.New("SSH access-key directory must be absolute")
	}
	if err := secureDirectory(root); err != nil {
		return nil, fmt.Errorf("prepare SSH access-key directory: %w", err)
	}
	return &SSHAccessStore{root: filepath.Clean(root)}, nil
}

// Ensure returns the stable key for one reservation, creating it atomically
// when no key exists. The private key is never returned to callers.
func (keys *SSHAccessStore) Ensure(reservationID string) (SSHAccessKey, error) {
	if keys == nil || !store.ValidID(reservationID) {
		return SSHAccessKey{}, errors.New("invalid SSH access-key reservation")
	}
	file := filepath.Join(keys.root, reservationID+".key")
	if key, err := loadSSHAccessKey(file, reservationID); err == nil {
		return key, nil
	} else if !errors.Is(err, os.ErrNotExist) {
		return SSHAccessKey{}, err
	}
	publicKey, privateKey, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		return SSHAccessKey{}, errors.New("generate reservation SSH key")
	}
	privateDER, err := x509.MarshalPKCS8PrivateKey(privateKey)
	clear(privateKey)
	if err != nil {
		return SSHAccessKey{}, errors.New("encode reservation SSH private key")
	}
	privatePEM := pem.EncodeToMemory(&pem.Block{Type: "PRIVATE KEY", Bytes: privateDER})
	clear(privateDER)
	temporary, err := os.CreateTemp(keys.root, "."+reservationID+"-*.tmp")
	if err != nil {
		clear(privatePEM)
		return SSHAccessKey{}, errors.New("create temporary reservation SSH key")
	}
	temporaryPath := temporary.Name()
	defer func() { _ = os.Remove(temporaryPath) }()
	if err := temporary.Chmod(0o600); err != nil {
		_ = temporary.Close()
		clear(privatePEM)
		return SSHAccessKey{}, errors.New("protect temporary reservation SSH key")
	}
	writeErr := writeAndClose(temporary, privatePEM)
	clear(privatePEM)
	if writeErr != nil {
		return SSHAccessKey{}, writeErr
	}
	if err := os.Link(temporaryPath, file); err != nil {
		if errors.Is(err, os.ErrExist) {
			return loadSSHAccessKey(file, reservationID)
		}
		return SSHAccessKey{}, errors.New("publish reservation SSH private key")
	}
	return SSHAccessKey{PublicKey: opensshEd25519PublicKey(publicKey, reservationID),
		PrivateKeyFile: file}, nil
}

// Load returns an existing reservation key and never rotates or creates it.
func (keys *SSHAccessStore) Load(reservationID string) (SSHAccessKey, error) {
	if keys == nil || !store.ValidID(reservationID) {
		return SSHAccessKey{}, errors.New("invalid SSH access-key reservation")
	}
	return loadSSHAccessKey(filepath.Join(keys.root, reservationID+".key"), reservationID)
}

// Remove deletes only the protected private key for the exact reservation.
func (keys *SSHAccessStore) Remove(reservationID string) error {
	if keys == nil || !store.ValidID(reservationID) {
		return errors.New("invalid SSH access-key reservation")
	}
	file := filepath.Join(keys.root, reservationID+".key")
	info, err := os.Lstat(file)
	if errors.Is(err, os.ErrNotExist) {
		return nil
	}
	if err != nil || !info.Mode().IsRegular() || info.Mode()&os.ModeSymlink != 0 ||
		info.Mode().Perm()&0o077 != 0 {
		return errors.New("refusing to remove an unsafe SSH key file")
	}
	if err := os.Remove(file); err != nil {
		return errors.New("remove reservation SSH private key")
	}
	return nil
}

func loadSSHAccessKey(file, reservationID string) (SSHAccessKey, error) {
	info, err := os.Lstat(file)
	if err != nil {
		return SSHAccessKey{}, err
	}
	if !info.Mode().IsRegular() || info.Mode()&os.ModeSymlink != 0 ||
		info.Size() < 1 || info.Size() > maxSSHPrivateKeyBytes ||
		info.Mode().Perm()&0o077 != 0 {
		return SSHAccessKey{}, errors.New("reservation SSH key violates private-file policy")
	}
	handle, err := os.Open(file)
	if err != nil {
		return SSHAccessKey{}, errors.New("read reservation SSH private key")
	}
	opened, err := handle.Stat()
	if err != nil || !opened.Mode().IsRegular() || !os.SameFile(info, opened) ||
		opened.Size() != info.Size() || opened.Size() < 1 || opened.Size() > maxSSHPrivateKeyBytes ||
		opened.Mode().Perm()&0o077 != 0 {
		_ = handle.Close()
		return SSHAccessKey{}, errors.New("reservation SSH key changed or violates private-file policy")
	}
	raw, err := io.ReadAll(io.LimitReader(handle, maxSSHPrivateKeyBytes+1))
	closeErr := handle.Close()
	if err != nil || closeErr != nil || int64(len(raw)) != opened.Size() {
		clear(raw)
		return SSHAccessKey{}, errors.New("read reservation SSH private key")
	}
	defer clear(raw)
	block, rest := pem.Decode(raw)
	if block == nil || len(rest) != 0 || block.Type != "PRIVATE KEY" {
		return SSHAccessKey{}, errors.New("reservation SSH private key is malformed")
	}
	parsed, err := x509.ParsePKCS8PrivateKey(block.Bytes)
	if err != nil {
		return SSHAccessKey{}, errors.New("parse reservation SSH private key")
	}
	privateKey, ok := parsed.(ed25519.PrivateKey)
	if !ok || len(privateKey) != ed25519.PrivateKeySize {
		return SSHAccessKey{}, errors.New("reservation SSH key has unexpected type")
	}
	publicKey, ok := privateKey.Public().(ed25519.PublicKey)
	clear(privateKey)
	if !ok || len(publicKey) != ed25519.PublicKeySize {
		return SSHAccessKey{}, errors.New("reservation SSH key has invalid public half")
	}
	return SSHAccessKey{PublicKey: opensshEd25519PublicKey(publicKey, reservationID),
		PrivateKeyFile: file}, nil
}

func opensshEd25519PublicKey(publicKey ed25519.PublicKey, reservationID string) string {
	typeName := []byte("ssh-ed25519")
	wire := make([]byte, 4+len(typeName)+4+len(publicKey))
	binary.BigEndian.PutUint32(wire[0:4], uint32(len(typeName)))
	copy(wire[4:], typeName)
	offset := 4 + len(typeName)
	binary.BigEndian.PutUint32(wire[offset:offset+4], uint32(len(publicKey)))
	copy(wire[offset+4:], publicKey)
	return "ssh-ed25519 " + base64.StdEncoding.EncodeToString(wire) + " ra8ci-" + reservationID
}

func writeAndClose(handle *os.File, content []byte) error {
	written, writeErr := handle.Write(content)
	syncErr := handle.Sync()
	closeErr := handle.Close()
	if writeErr != nil || syncErr != nil || closeErr != nil || written != len(content) {
		return errors.New("write reservation SSH private key")
	}
	return nil
}
