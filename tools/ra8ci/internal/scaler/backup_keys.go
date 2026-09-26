// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package scaler

import (
	"crypto/ed25519"
	"crypto/rand"
	"encoding/base64"
	"errors"
	"os"
	"path/filepath"
	"strings"
)

// CreateBackupSigningKeyPair creates owner-only base64 key files without
// replacing existing material. Deployment assigns the private file to the
// isolated monitor group and the public file to the API service group.
func CreateBackupSigningKeyPair(privatePath, publicPath string) error {
	if !filepath.IsAbs(privatePath) || !filepath.IsAbs(publicPath) ||
		filepath.Clean(privatePath) != privatePath || filepath.Clean(publicPath) != publicPath || privatePath == publicPath {
		return errors.New("invalid backup signing key paths")
	}
	for _, path := range []string{privatePath, publicPath} {
		info, err := os.Lstat(filepath.Dir(path))
		if err != nil || !info.IsDir() || info.Mode()&os.ModeSymlink != 0 {
			return errors.New("backup signing key parent must be a protected real directory")
		}
		// Exclusive creation refuses to replace an existing key, but it
		// cannot stop an account that can write the directory from renaming
		// the new key away and leaving its own pair at the path, which keys
		// the gate to an attacker from the moment it is generated.
		if info.Mode().Perm()&0022 != 0 {
			return errors.New("backup signing key parent must not be group or world writable")
		}
		if _, err := os.Lstat(path); err == nil || !os.IsNotExist(err) {
			return errors.New("refusing to replace existing backup signing key")
		}
	}
	publicKey, privateKey, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		return errors.New("generate backup signing key pair")
	}
	defer clear(privateKey)
	privateBytes := []byte(base64.StdEncoding.EncodeToString(privateKey) + "\n")
	defer clear(privateBytes)
	publicBytes := []byte(base64.StdEncoding.EncodeToString(publicKey) + "\n")
	if err := writeNewPrivateFile(privatePath, privateBytes, 0o600); err != nil {
		return err
	}
	if err := writeNewPrivateFile(publicPath, publicBytes, 0o600); err != nil {
		_ = os.Remove(privatePath)
		return err
	}
	return nil
}

func writeNewPrivateFile(path string, contents []byte, mode os.FileMode) error {
	file, err := os.OpenFile(path, os.O_WRONLY|os.O_CREATE|os.O_EXCL, mode)
	if err != nil {
		return errors.New("create backup key file without replacement")
	}
	if _, err := file.Write(contents); err != nil {
		_ = file.Close()
		_ = os.Remove(path)
		return errors.New("write backup key file")
	}
	if err := file.Sync(); err != nil {
		_ = file.Close()
		_ = os.Remove(path)
		return errors.New("sync backup key file")
	}
	if err := file.Close(); err != nil {
		_ = os.Remove(path)
		return errors.New("close backup key file")
	}
	return nil
}

func validBackupStanza(value string) bool {
	if value == "" || len(value) > 64 || strings.TrimSpace(value) != value {
		return false
	}
	for index, character := range value {
		alphaNumeric := character >= 'a' && character <= 'z' || character >= 'A' && character <= 'Z' || character >= '0' && character <= '9'
		if !alphaNumeric && character != '_' && character != '-' || index == 0 && !alphaNumeric {
			return false
		}
	}
	return true
}
