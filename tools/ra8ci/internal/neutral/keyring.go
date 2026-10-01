// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package neutral

import (
	"bytes"
	"crypto/ed25519"
	"encoding/base64"
	"encoding/json"
	"errors"
	"io"
	"os"
)

const maxVerifierKeyringBytes = 64 << 10

type verifierKeyring struct {
	SchemaVersion int                    `json:"schema_version"`
	Agents        []verifierKeyringAgent `json:"agents"`
}

type verifierKeyringAgent struct {
	KeyID           string `json:"key_id"`
	PublicKeyBase64 string `json:"public_key_base64"`
}

// LoadVerifierFile loads an immutable allowlist of board-agent Ed25519 public
// keys. The file must be a protected regular file without group/other write
// permissions; deployments should install it root-owned and read-only to ra8ci.
func LoadVerifierFile(path string) (*Verifier, error) {
	if path == "" {
		return nil, errors.New("board-agent verifier keyring path is empty")
	}
	info, err := os.Lstat(path)
	if err != nil || !info.Mode().IsRegular() || info.Size() <= 0 ||
		info.Size() > maxVerifierKeyringBytes || info.Mode().Perm()&0022 != 0 {
		return nil, errors.New("board-agent verifier keyring must be a bounded regular file not writable by group or other")
	}
	file, err := os.Open(path)
	if err != nil {
		return nil, errors.New("open board-agent verifier keyring")
	}
	defer file.Close()
	opened, err := file.Stat()
	if err != nil || !opened.Mode().IsRegular() || !os.SameFile(info, opened) ||
		opened.Size() <= 0 || opened.Size() > maxVerifierKeyringBytes ||
		opened.Mode().Perm()&0022 != 0 {
		return nil, errors.New("board-agent verifier keyring changed or became unsafe")
	}
	raw, err := io.ReadAll(io.LimitReader(file, maxVerifierKeyringBytes+1))
	if err != nil || len(raw) == 0 || len(raw) > maxVerifierKeyringBytes {
		return nil, errors.New("read bounded board-agent verifier keyring")
	}
	defer clear(raw)

	decoder := json.NewDecoder(bytes.NewReader(raw))
	decoder.DisallowUnknownFields()
	var keyring verifierKeyring
	if err := decoder.Decode(&keyring); err != nil || keyring.SchemaVersion != 1 ||
		len(keyring.Agents) == 0 || len(keyring.Agents) > 256 {
		return nil, errors.New("invalid board-agent verifier keyring")
	}
	var trailing any
	if err := decoder.Decode(&trailing); err != io.EOF {
		return nil, errors.New("board-agent verifier keyring has trailing data")
	}
	keys := make(map[string]ed25519.PublicKey, len(keyring.Agents))
	for _, agent := range keyring.Agents {
		if !validKeyID(agent.KeyID) {
			return nil, errors.New("invalid board-agent key ID in verifier keyring")
		}
		if _, duplicate := keys[agent.KeyID]; duplicate {
			return nil, errors.New("duplicate board-agent key ID in verifier keyring")
		}
		publicKey, err := base64.StdEncoding.Strict().DecodeString(agent.PublicKeyBase64)
		if err != nil || len(publicKey) != ed25519.PublicKeySize {
			clear(publicKey)
			return nil, errors.New("invalid board-agent Ed25519 key in verifier keyring")
		}
		keys[agent.KeyID] = ed25519.PublicKey(publicKey)
	}
	verifier, err := NewVerifier(keys, nil)
	for _, key := range keys {
		clear(key)
	}
	if err != nil {
		return nil, errors.New("construct board-agent verifier from keyring")
	}
	return verifier, nil
}
