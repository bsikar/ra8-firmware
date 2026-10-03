package mtls

import (
	"errors"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/testprivatefile"
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
	if runtime.GOOS == "windows" {
		if err := testprivatefile.OwnerOnly(keyPath); err != nil {
			t.Fatalf("protect key: %v", err)
		}
	}
	return certPath, keyPath, now
}

func TestAnOwnerOnlyKeyFileIsAccepted(t *testing.T) {
	for _, mode := range []os.FileMode{0o600, 0o400} {
		certPath, keyPath, now := keyPairAtMode(t, mode)
		if _, err := LoadClientIdentity(certPath, keyPath, now); err != nil {
			t.Fatalf("mode %04o was refused: %v", mode, err)
		}
	}
}

func TestAGroupAccessibleKeyFileIsRefused(t *testing.T) {
	if runtime.GOOS == "windows" {
		t.Skip("Windows file privacy is checked through the DACL")
	}
	for _, mode := range []os.FileMode{0o640, 0o660} {
		certPath, keyPath, now := keyPairAtMode(t, mode)
		if _, err := LoadServerIdentity(certPath, keyPath, now); !errors.Is(err, ErrIdentity) {
			t.Fatalf("group-accessible key mode %04o was accepted", mode)
		}
	}
}

func TestAWorldReadableKeyFileIsRefused(t *testing.T) {
	if runtime.GOOS == "windows" {
		t.Skip("Windows file privacy is checked through the DACL")
	}
	for _, mode := range []os.FileMode{0o644, 0o604, 0o666, 0o777, 0o602, 0o640} {
		certPath, keyPath, now := keyPairAtMode(t, mode)
		_, refusal := LoadClientIdentity(certPath, keyPath, now)
		if refusal == nil {
			t.Fatalf("mode %04o loaded", mode)
		}
		if !errors.Is(refusal, ErrIdentity) || !strings.Contains(refusal.Error(), "not owner-only") {
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
	if !errors.Is(err, ErrIdentity) || !strings.Contains(err.Error(), "not a regular file") {
		t.Fatalf("a directory key path: unexpected refusal %v", err)
	}
}
