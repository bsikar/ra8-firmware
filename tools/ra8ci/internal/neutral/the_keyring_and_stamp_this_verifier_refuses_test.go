// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package neutral

import (
	"context"
	"crypto/ed25519"
	"encoding/base64"
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"testing"
)

func writeKeyringFile(t *testing.T, keyring verifierKeyring) string {
	t.Helper()
	raw, err := json.Marshal(keyring)
	if err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(t.TempDir(), "board-agent-keys.json")
	if err := os.WriteFile(path, raw, 0o600); err != nil {
		t.Fatal(err)
	}
	return path
}

func TestLoadVerifierFileRefusesAnEmptyPath(t *testing.T) {
	// An unset keyring path is its own refusal, named before any file is
	// stat'ed, so an operator is told the setting is missing rather than
	// that some empty name could not be read.
	verifier, err := LoadVerifierFile("")
	if verifier != nil || err == nil {
		t.Fatalf("verifier = %v, error = %v", verifier, err)
	}
}

func TestLoadVerifierFileRefusesAKeyIDItCannotTrust(t *testing.T) {
	publicKey, _, err := ed25519.GenerateKey(nil)
	if err != nil {
		t.Fatal(err)
	}
	encoded := base64.StdEncoding.EncodeToString(publicKey)
	for _, keyID := range []string{"", "hil board agent", "hil/board/agent", "hil-board-agent-\u00e9"} {
		t.Run("id="+keyID, func(t *testing.T) {
			path := writeKeyringFile(t, verifierKeyring{SchemaVersion: 1,
				Agents: []verifierKeyringAgent{{KeyID: keyID, PublicKeyBase64: encoded}}})
			verifier, err := LoadVerifierFile(path)
			if verifier != nil || err == nil {
				t.Fatalf("verifier = %v, error = %v", verifier, err)
			}
		})
	}
}

func TestVerifierRefusesAnObservationStampItCannotRead(t *testing.T) {
	// The stamp is parsed BEFORE the signature is checked, so a receipt
	// that is cryptographically sound but carries an unreadable time is
	// refused on the time. The signature here is left intact to prove the
	// refusal is not the signature's.
	challenge, _, public, _, raw := receiptFixture(t)
	var receipt Receipt
	if err := json.Unmarshal(raw, &receipt); err != nil {
		t.Fatal(err)
	}
	if len(receipt.Signature) != ed25519.SignatureSize {
		t.Fatalf("fixture signature is %d bytes", len(receipt.Signature))
	}
	receipt.Payload.ObservedAt = "the moment the board went quiet"
	tampered, err := json.Marshal(receipt)
	if err != nil {
		t.Fatal(err)
	}
	if err := testVerifier(t, public).VerifyNeutralReceipt(context.Background(), challenge, tampered); !errors.Is(err, ErrInvalidReceipt) {
		t.Fatalf("error = %v", err)
	}
}
