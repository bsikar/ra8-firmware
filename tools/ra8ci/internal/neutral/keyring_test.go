// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package neutral

import (
	"crypto/ed25519"
	"encoding/base64"
	"encoding/json"
	"os"
	"path/filepath"
	"testing"
)

func TestLoadVerifierFileCopiesStrictPublicKeyAllowlist(t *testing.T) {
	publicKey, _, err := ed25519.GenerateKey(nil)
	if err != nil {
		t.Fatal(err)
	}
	expectedKey := append(ed25519.PublicKey(nil), publicKey...)
	expected := verifierKeyring{SchemaVersion: 1, Agents: []verifierKeyringAgent{{
		KeyID: "hil-board-agent-1", PublicKeyBase64: base64.StdEncoding.EncodeToString(publicKey),
	}}}
	raw, err := json.Marshal(expected)
	if err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(t.TempDir(), "board-agent-keys.json")
	if err := os.WriteFile(path, raw, 0o600); err != nil {
		t.Fatal(err)
	}
	verifier, err := LoadVerifierFile(path)
	if err != nil {
		t.Fatal(err)
	}
	loaded := verifier.keys[expected.Agents[0].KeyID]
	if len(loaded) != ed25519.PublicKeySize || string(loaded) != string(publicKey) {
		t.Fatal("loaded verifier key differs from configured public key")
	}
	clear(publicKey)
	if string(verifier.keys[expected.Agents[0].KeyID]) != string(expectedKey) {
		t.Fatal("verifier did not retain an independent key copy")
	}
	clear(expectedKey)
}

func TestLoadVerifierFileRejectsMalformedOrMutableKeyrings(t *testing.T) {
	cases := []struct {
		name string
		raw  string
		mode os.FileMode
	}{
		{name: "wrong schema", raw: `{"schema_version":2,"agents":[{"key_id":"agent-1","public_key_base64":"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="}]}`, mode: 0o600},
		{name: "duplicate ID", raw: `{"schema_version":1,"agents":[{"key_id":"agent-1","public_key_base64":"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="},{"key_id":"agent-1","public_key_base64":"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="}]}`, mode: 0o600},
		{name: "invalid key length", raw: `{"schema_version":1,"agents":[{"key_id":"agent-1","public_key_base64":"AA=="}]}`, mode: 0o600},
		{name: "unknown field", raw: `{"schema_version":1,"agents":[],"extra":true}`, mode: 0o600},
		{name: "trailing document", raw: `{"schema_version":1,"agents":[{"key_id":"agent-1","public_key_base64":"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="}]} {}`, mode: 0o600},
		{name: "group writable", raw: `{"schema_version":1,"agents":[{"key_id":"agent-1","public_key_base64":"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="}]}`, mode: 0o620},
	}
	for _, test := range cases {
		t.Run(test.name, func(t *testing.T) {
			path := filepath.Join(t.TempDir(), "keyring.json")
			if err := os.WriteFile(path, []byte(test.raw), test.mode); err != nil {
				t.Fatal(err)
			}
			if err := os.Chmod(path, test.mode); err != nil {
				t.Fatal(err)
			}
			if _, err := LoadVerifierFile(path); err == nil {
				t.Fatal("unsafe board-agent keyring was accepted")
			}
		})
	}
}
