package mtls

import (
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

// keyPairAtMode writes a live client key pair to disk with the key file at the
// given mode and returns the two paths.
func keyPairAtMode(t *testing.T, mode os.FileMode) (string, string, time.Time) {
	t.Helper()
	now := time.Now()
	certPEM, keyPEM := clientKeyPairPEM(t, now.Add(-time.Hour), now.Add(time.Hour))
	certPath, keyPath := writeKeyPair(t, certPEM, keyPEM)
	if err := os.Chmod(keyPath, mode); err != nil {
		t.Fatalf("chmod key: %v", err)
	}
	return certPath, keyPath, now
}

func TestAnOwnerOnlyKeyFileIsAccepted(t *testing.T) {
	for _, mode := range []os.FileMode{0o600, 0o400, 0o640, 0o660} {
		certPath, keyPath, now := keyPairAtMode(t, mode)
		if _, err := LoadClientIdentity(certPath, keyPath, now); err != nil {
			t.Fatalf("mode %04o was refused: %v", mode, err)
		}
	}
}

// The judgement this test pins: a group-readable key is a deliberate and common
// arrangement (owned by the account that provisions it, read by the group the
// runner runs as), so refusing it would lock out running deployments to say
// nothing the operator did not already decide.
func TestAGroupReadableKeyFileIsNotRefused(t *testing.T) {
	certPath, keyPath, now := keyPairAtMode(t, 0o640)
	if _, err := LoadServerIdentity(certPath, keyPath, now); err != nil && errors.Is(err, ErrIdentity) &&
		strings.Contains(err.Error(), "readable by every account") {
		t.Fatalf("a group-readable key was refused: %v", err)
	}
}

func TestAWorldReadableKeyFileIsRefused(t *testing.T) {
	for _, mode := range []os.FileMode{0o644, 0o604, 0o666, 0o777, 0o602} {
		certPath, keyPath, now := keyPairAtMode(t, mode)
		_, refusal := LoadClientIdentity(certPath, keyPath, now)
		if refusal == nil {
			t.Fatalf("mode %04o loaded", mode)
		}
		if !errors.Is(refusal, ErrIdentity) || !strings.Contains(refusal.Error(), "readable by every account") {
			t.Fatalf("mode %04o: unexpected refusal %v", mode, refusal)
		}
		if !strings.Contains(refusal.Error(), keyPath) {
			t.Fatalf("mode %04o: the refusal does not name the file: %v", mode, refusal)
		}
	}
}

// Both ends load a key the same way, so the listener is held to the same rule.
func TestTheServerKeyFileIsHeldToTheSameRule(t *testing.T) {
	certPath, keyPath, now := keyPairAtMode(t, 0o644)
	if _, err := LoadServerIdentity(certPath, keyPath, now); !errors.Is(err, ErrIdentity) {
		t.Fatalf("a world-readable server key loaded: %v", err)
	}
}

// The certificate is public and its mode decides nothing, so a world-readable
// certificate beside an owner-only key is left alone.
func TestTheCertificateFileModeIsNotJudged(t *testing.T) {
	certPath, keyPath, now := keyPairAtMode(t, 0o600)
	if err := os.Chmod(certPath, 0o644); err != nil {
		t.Fatalf("chmod certificate: %v", err)
	}
	if _, err := LoadClientIdentity(certPath, keyPath, now); err != nil {
		t.Fatalf("a world-readable certificate was refused: %v", err)
	}
}

// The bytes handed to the TLS stack come from the file at the end of a symlink,
// so that file's permissions are the ones that decide the question.
func TestAKeyReachedThroughASymlinkIsJudgedAtItsTarget(t *testing.T) {
	certPath, keyPath, now := keyPairAtMode(t, 0o644)
	linked := filepath.Join(t.TempDir(), "identity.key")
	if err := os.Symlink(keyPath, linked); err != nil {
		t.Skipf("symlinks are unavailable here: %v", err)
	}
	if _, err := LoadClientIdentity(certPath, linked, now); !errors.Is(err, ErrIdentity) {
		t.Fatalf("a world-readable key behind a symlink loaded: %v", err)
	}
}

func TestAnAbsentOrDirectoryKeyPathIsRefusedBeforeTheRead(t *testing.T) {
	certPath, _, now := keyPairAtMode(t, 0o600)
	absent := filepath.Join(t.TempDir(), "absent.key")
	if _, err := LoadClientIdentity(certPath, absent, now); !errors.Is(err, ErrIdentity) {
		t.Fatalf("an absent key path was not refused: %v", err)
	}
	dir := t.TempDir()
	_, err := LoadClientIdentity(certPath, dir, now)
	if !errors.Is(err, ErrIdentity) || !strings.Contains(err.Error(), "is a directory") {
		t.Fatalf("a directory key path: unexpected refusal %v", err)
	}
}

// The rule reads the permission bits and nothing else, so a test that wants to
// know what it saw reads them the same way rather than restating the mask.
func TestTheRuleReadsThePermissionBitsItReports(t *testing.T) {
	_, keyPath, _ := keyPairAtMode(t, 0o604)
	info, err := os.Stat(keyPath)
	if err != nil {
		t.Fatalf("stat: %v", err)
	}
	if permissionsOf(info)&0o007 == 0 {
		t.Fatalf("expected world bits on %s, saw %04o", keyPath, permissionsOf(info))
	}
	if permissionsOf(nil) != 0 {
		t.Fatal("a missing file info reports permissions")
	}
	if err := checkPrivateKeyFileMode(keyPath); err == nil {
		t.Fatal("the rule accepted a world-readable key")
	}
}
