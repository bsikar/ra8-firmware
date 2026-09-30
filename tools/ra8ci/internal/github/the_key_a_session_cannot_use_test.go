// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"context"
	"os"
	"strings"
	"testing"
)

// The last local doors on the way to a GitHub App session, all of them after
// the key file has already satisfied every check made against its metadata.
// session_test.go and the_door_a_session_opens_test.go pin the metadata half:
// the owner, the nil caller, a group-readable key, a directory, a symlink, an
// empty key, an oversized key and an absent one. What is left is the file that
// passes all of that and still cannot become a credential.

// A key file can be a bounded regular file readable by nobody but its owner
// and still refuse to open. Mode 0o000 satisfies every check made against the
// metadata, so the refusal has to come from the open itself. An operator who
// sealed a key and forgot needs to be told the file could not be read, not
// that GitHub rejected the credential.
func TestAPrivateKeyThatCannotBeOpenedIsRefusedBeforeTheCredential(t *testing.T) {
	config := openable(t, []byte("placeholder key material"))
	if err := os.Chmod(config.PrivateKeyFile, 0o000); err != nil {
		t.Fatal(err)
	}
	if readable, err := os.ReadFile(config.PrivateKeyFile); err == nil {
		t.Skipf("this process reads a mode 0000 file (%d bytes); the seal proves nothing here", len(readable))
	}

	_, err := OpenSession(context.Background(), config)
	if err == nil || !strings.Contains(err.Error(), "read GitHub App private key") {
		t.Fatalf("a sealed key file: %v", err)
	}
	// Every metadata check passed, so neither of their refusals is what
	// answered here.
	if strings.Contains(err.Error(), "bounded regular file") || strings.Contains(err.Error(), "group or others") {
		t.Fatalf("the open was blamed on the file's metadata: %v", err)
	}
}

// The App credential is assembled from three fields and validated by shape
// alone: a client ID, an installation and some bytes. Bytes that are not a
// PEM key satisfy that shape, so the refusal arrives one step later, when the
// scale set is verified and the JWT cannot be signed. Pinning it here records
// which step actually speaks, because an operator reading "verify configured
// GitHub scale set" would otherwise go looking at the scale set rather than
// at the key they installed.
func TestKeyMaterialThatIsNotAKeyIsRefusedWhenTheScaleSetIsVerified(t *testing.T) {
	_, err := OpenSession(context.Background(), openable(t, []byte("placeholder key material")))
	if err == nil {
		t.Fatal("a session was opened on key material that is not a key")
	}
	if !strings.Contains(err.Error(), "verify configured GitHub scale set") {
		t.Fatalf("the refusal did not come from the scale-set check: %v", err)
	}
	// The cause is carried through rather than flattened, so the message
	// names the key material itself.
	if !strings.Contains(err.Error(), "PEM") {
		t.Fatalf("the refusal did not name the key material: %v", err)
	}
	// It is not reported as a problem with the file, which is readable and
	// within bounds.
	if strings.Contains(err.Error(), "read GitHub App private key") ||
		strings.Contains(err.Error(), "bounded regular file") {
		t.Fatalf("readable key material was reported as a file problem: %v", err)
	}
}

// Three shapes of unusable key material, none of which yields a session and
// all of which are refused at the same door. A signing key that cannot be
// parsed stops the session locally: no token is minted, so there is nothing
// to send.
func TestNoSessionIsBuiltOnKeyMaterialThatCannotSign(t *testing.T) {
	for name, key := range map[string][]byte{
		"not a key":    []byte("placeholder key material"),
		"a PEM header": []byte("-----BEGIN RSA PRIVATE KEY-----\nnot base64\n-----END RSA PRIVATE KEY-----\n"),
		"whitespace":   []byte("   \n\t\n"),
	} {
		t.Run(name, func(t *testing.T) {
			session, err := OpenSession(context.Background(), openable(t, key))
			if err == nil {
				t.Fatal("a session was opened on key material that cannot sign")
			}
			if session != nil {
				t.Fatal("a refused session was handed back anyway")
			}
			if !strings.Contains(err.Error(), "verify configured GitHub scale set") {
				t.Fatalf("a different door answered: %v", err)
			}
		})
	}
}
