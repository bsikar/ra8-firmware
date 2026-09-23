// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package scaler

import (
	"crypto/ed25519"
	"encoding/base64"
	"os"
	"path/filepath"
	"testing"
)

func TestCreateBackupSigningKeyPairIsExclusiveAndParseable(t *testing.T) {
	dir := t.TempDir()
	privatePath, publicPath := filepath.Join(dir, "private.key"), filepath.Join(dir, "public.key")
	if err := CreateBackupSigningKeyPair(privatePath, publicPath); err != nil {
		t.Fatal(err)
	}
	privateRaw, err := os.ReadFile(privatePath)
	if err != nil {
		t.Fatal(err)
	}
	defer clear(privateRaw)
	privateBytes, err := base64.StdEncoding.Strict().DecodeString(string(privateRaw[:len(privateRaw)-1]))
	if err != nil || len(privateBytes) != ed25519.PrivateKeySize {
		t.Fatalf("private key invalid: %v", err)
	}
	defer clear(privateBytes)
	publicRaw, err := os.ReadFile(publicPath)
	if err != nil {
		t.Fatal(err)
	}
	publicBytes, err := base64.StdEncoding.Strict().DecodeString(string(publicRaw[:len(publicRaw)-1]))
	if err != nil || len(publicBytes) != ed25519.PublicKeySize {
		t.Fatalf("public key invalid: %v", err)
	}
	loadedPublic, err := LoadBackupPublicKey(publicPath)
	if err != nil || string(loadedPublic) != string(publicBytes) {
		t.Fatalf("public key loader = %x, %v", loadedPublic, err)
	}
	before := append([]byte(nil), privateRaw...)
	if err := CreateBackupSigningKeyPair(privatePath, publicPath); err == nil {
		t.Fatal("existing key pair was replaced")
	}
	after, _ := os.ReadFile(privatePath)
	if string(before) != string(after) {
		t.Fatal("private key changed on rejected replacement")
	}
}
