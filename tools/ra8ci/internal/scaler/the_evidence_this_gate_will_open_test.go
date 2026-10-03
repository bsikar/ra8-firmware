// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package scaler

import (
	"context"
	"crypto/ed25519"
	"encoding/base64"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

// The tests beside this file judge the gate's dates: staleness, skew, and
// evidence dated after the check that signed it. This one judges everything
// before a date is read. The gate is the only thing standing between a stale
// or forged backup record and new runner capacity, so its configuration
// bounds, the file it will open, and the envelope it will decode are each
// pinned here.

func gateRefusal(t *testing.T, gate *SignedBackupGate, want string) {
	t.Helper()
	err := gate.Check(context.Background(), gate.approvalID)
	if err == nil {
		t.Fatal("unsound backup evidence was accepted")
	}
	if !strings.Contains(err.Error(), want) {
		t.Fatalf("refused for the wrong reason: %v", err)
	}
}

// writeRawAttestation publishes bytes at the gate's path with the 0640 the
// file rule expects, so a refusal can only come from the content.
func writeRawAttestation(t *testing.T, gate *SignedBackupGate, raw []byte) {
	t.Helper()
	if err := os.WriteFile(gate.path, raw, 0o600); err != nil {
		t.Fatal(err)
	}
	if err := os.Chmod(gate.path, 0o640); err != nil {
		t.Fatal(err)
	}
}

// Every bound here is policy, not taste: a check window over an hour, a
// backup window over a week, or a drill window over a year would let the gate
// pass evidence nobody would call current.
func TestAGateConfiguredOutsideItsPolicyBoundsIsRefused(t *testing.T) {
	public, _, err := ed25519.GenerateKey(nil)
	if err != nil {
		t.Fatal(err)
	}
	const approval = "018d1234-5678-7abc-8def-123456789abc"
	sound := filepath.Join(t.TempDir(), "backup.json")
	for name, build := range map[string]func() (*SignedBackupGate, error){
		"a relative path": func() (*SignedBackupGate, error) {
			return NewSignedBackupGate("backup.json", public, approval, time.Hour, 48*time.Hour, 90*24*time.Hour)
		},
		"an unclean path": func() (*SignedBackupGate, error) {
			return NewSignedBackupGate(filepath.Dir(sound)+"/./backup.json", public, approval,
				time.Hour, 48*time.Hour, 90*24*time.Hour)
		},
		"a public key of the wrong length": func() (*SignedBackupGate, error) {
			return NewSignedBackupGate(sound, public[:16], approval, time.Hour, 48*time.Hour, 90*24*time.Hour)
		},
		"no public key at all": func() (*SignedBackupGate, error) {
			return NewSignedBackupGate(sound, nil, approval, time.Hour, 48*time.Hour, 90*24*time.Hour)
		},
		"an approval identity that is not an identifier": func() (*SignedBackupGate, error) {
			return NewSignedBackupGate(sound, public, "backups", time.Hour, 48*time.Hour, 90*24*time.Hour)
		},
		"no check window": func() (*SignedBackupGate, error) {
			return NewSignedBackupGate(sound, public, approval, 0, 48*time.Hour, 90*24*time.Hour)
		},
		"a check window past an hour": func() (*SignedBackupGate, error) {
			return NewSignedBackupGate(sound, public, approval, time.Hour+time.Second, 48*time.Hour, 90*24*time.Hour)
		},
		"a negative backup window": func() (*SignedBackupGate, error) {
			return NewSignedBackupGate(sound, public, approval, time.Hour, -time.Hour, 90*24*time.Hour)
		},
		"a backup window past a week": func() (*SignedBackupGate, error) {
			return NewSignedBackupGate(sound, public, approval, time.Hour, 7*24*time.Hour+time.Second, 90*24*time.Hour)
		},
		"a drill window past a year": func() (*SignedBackupGate, error) {
			return NewSignedBackupGate(sound, public, approval, time.Hour, 48*time.Hour, 365*24*time.Hour+time.Second)
		},
	} {
		gate, err := build()
		if err == nil {
			t.Fatalf("%s was accepted", name)
		}
		if gate != nil {
			t.Fatalf("%s returned a usable gate", name)
		}
		if !strings.Contains(err.Error(), "invalid signed backup gate configuration") {
			t.Fatalf("%s refused for the wrong reason: %v", name, err)
		}
	}
}

// Each window is accepted exactly at its ceiling, which is the pair that says
// the bounds above are the policy and not an off-by-one.
func TestAGateAtEveryPolicyCeilingIsStillBuilt(t *testing.T) {
	public, _, err := ed25519.GenerateKey(nil)
	if err != nil {
		t.Fatal(err)
	}
	gate, err := NewSignedBackupGate(filepath.Join(t.TempDir(), "backup.json"), public,
		"018d1234-5678-7abc-8def-123456789abc", time.Hour, 7*24*time.Hour, 365*24*time.Hour)
	if err != nil {
		t.Fatalf("a gate at every ceiling was refused: %v", err)
	}
	if gate == nil {
		t.Fatal("no gate was returned")
	}
}

// The gate copies the key it was handed. A caller that reuses that slice
// afterwards must not be able to change what the gate will believe.
func TestAGateKeepsItsOwnCopyOfTheVerifyingKey(t *testing.T) {
	gate, attestation, key := backupGateFixture(t)
	writeSignedBackupFixture(t, gate.path, attestation, key)
	handed := gate.publicKey
	gate.publicKey = append(ed25519.PublicKey(nil), handed...)
	for i := range handed {
		handed[i] = 0
	}
	if err := gate.Check(context.Background(), gate.approvalID); err != nil {
		t.Fatalf("clearing the caller's slice changed the gate's answer: %v", err)
	}
}

func TestACheckWithNoGateOrNoContextNamesThePolicy(t *testing.T) {
	var absent *SignedBackupGate
	if err := absent.Check(context.Background(), "018d1234-5678-7abc-8def-123456789abc"); err == nil {
		t.Fatal("a nil gate answered a check")
	}
	gate, attestation, key := backupGateFixture(t)
	writeSignedBackupFixture(t, gate.path, attestation, key)
	var absentContext context.Context
	err := gate.Check(absentContext, gate.approvalID)
	if err == nil {
		t.Fatal("a check with no context was accepted")
	}
	if !strings.Contains(err.Error(), "approval does not match configured policy") {
		t.Fatalf("refused for the wrong reason: %v", err)
	}
}

// A cancelled check hands back the cancellation itself rather than a policy
// refusal, so a caller can tell a shutdown from missing backup evidence.
func TestACancelledCheckIsHandedBackAsTheCancellation(t *testing.T) {
	gate, attestation, key := backupGateFixture(t)
	writeSignedBackupFixture(t, gate.path, attestation, key)
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	err := gate.Check(ctx, gate.approvalID)
	if !errors.Is(err, context.Canceled) {
		t.Fatalf("a cancelled check answered %v, want context.Canceled", err)
	}
}

func TestAnAbsentAttestationIsReportedAsAFailedStat(t *testing.T) {
	gate, _, _ := backupGateFixture(t)
	gateRefusal(t, gate, "stat signed backup attestation")
}

func TestAnAttestationThatIsADirectoryIsRefusedOnItsShape(t *testing.T) {
	gate, _, _ := backupGateFixture(t)
	if err := os.Mkdir(gate.path, 0o750); err != nil {
		t.Fatal(err)
	}
	gateRefusal(t, gate, "backup attestation must be a bounded, non-writable regular file")
}

func TestAnEmptyAttestationIsRefusedOnItsShape(t *testing.T) {
	gate, _, _ := backupGateFixture(t)
	writeRawAttestation(t, gate, nil)
	gateRefusal(t, gate, "backup attestation must be a bounded, non-writable regular file")
}

// A symlink at the path is a redirection of what the gate reads, caught by
// Lstat before the file behind it is ever opened.
func TestAnAttestationThatIsASymlinkIsRefusedOnItsShape(t *testing.T) {
	gate, attestation, key := backupGateFixture(t)
	behind := filepath.Join(filepath.Dir(gate.path), "behind.json")
	writeSignedBackupFixture(t, behind, attestation, key)
	symlinkTest(t, behind, gate.path)
	gateRefusal(t, gate, "backup attestation must be a bounded, non-writable regular file")
}

func TestAnAttestationOverTheSizeBoundIsRefusedOnItsShape(t *testing.T) {
	gate, attestation, key := backupGateFixture(t)
	raw, err := SignBackupAttestation(attestation, key)
	if err != nil {
		t.Fatal(err)
	}
	padded := append(raw, []byte(strings.Repeat("\n", maxBackupAttestationBytes))...)
	writeRawAttestation(t, gate, padded)
	gateRefusal(t, gate, "backup attestation must be a bounded, non-writable regular file")
}

func TestAnAttestationThatIsNotOneJSONDocumentIsRefused(t *testing.T) {
	gate, attestation, key := backupGateFixture(t)
	raw, err := SignBackupAttestation(attestation, key)
	if err != nil {
		t.Fatal(err)
	}
	for name, body := range map[string][]byte{
		"prose":              []byte("the backup is fine\n"),
		"a truncated object": raw[:len(raw)/2],
		"an array":           []byte("[" + strings.TrimRight(string(raw), "\n") + "]\n"),
	} {
		writeRawAttestation(t, gate, body)
		err := gate.Check(context.Background(), gate.approvalID)
		if err == nil {
			t.Fatalf("%s was accepted as an attestation", name)
		}
		if !strings.Contains(err.Error(), "decode signed backup attestation") {
			t.Fatalf("%s refused for the wrong reason: %v", name, err)
		}
	}
}

// A second envelope behind a sound first one is how a replaced record hides,
// so it is named rather than quietly ignored by the decoder.
func TestAnAttestationWithASecondEnvelopeIsRefusedForTheTrailingJSON(t *testing.T) {
	gate, attestation, key := backupGateFixture(t)
	raw, err := SignBackupAttestation(attestation, key)
	if err != nil {
		t.Fatal(err)
	}
	writeRawAttestation(t, gate, append(append([]byte(nil), raw...), raw...))
	gateRefusal(t, gate, "backup attestation has trailing JSON")
}

func TestAnAttestationCarryingAnUnknownFieldIsRefused(t *testing.T) {
	gate, attestation, key := backupGateFixture(t)
	raw, err := SignBackupAttestation(attestation, key)
	if err != nil {
		t.Fatal(err)
	}
	widened := strings.TrimRight(string(raw), "}\n") + `,"signed_by":"someone"}` + "\n"
	writeRawAttestation(t, gate, []byte(widened))
	gateRefusal(t, gate, "decode signed backup attestation")
}

// A signed envelope for another schema or another approval is refused on its
// identity before its signature is even decoded, because a gate that verified
// first would be verifying evidence it was never asked about.
func TestASignedAttestationForAnotherSchemaOrApprovalIsRefused(t *testing.T) {
	gate, attestation, key := backupGateFixture(t)
	foreignSchema := attestation
	foreignSchema.SchemaVersion = backupAttestationSchema + 1
	writeRawAttestation(t, gate, signRawAttestation(t, foreignSchema, key))
	gateRefusal(t, gate, "schema or approval identity mismatch")

	foreignApproval := attestation
	foreignApproval.ApprovalID = "018d1234-5678-7abc-8def-123456789abd"
	writeRawAttestation(t, gate, signRawAttestation(t, foreignApproval, key))
	gateRefusal(t, gate, "schema or approval identity mismatch")
}

func TestAnAttestationWithAMalformedSignatureIsRefused(t *testing.T) {
	gate, attestation, key := backupGateFixture(t)
	raw, err := SignBackupAttestation(attestation, key)
	if err != nil {
		t.Fatal(err)
	}
	signed := string(raw)
	opening := strings.Index(signed, `"signature":"`)
	if opening < 0 {
		t.Fatal("the signed envelope carries no signature field")
	}
	closing := strings.Index(signed[opening+13:], `"`)
	if closing < 0 {
		t.Fatal("the signature field is unterminated")
	}
	for name, replacement := range map[string]string{
		"an empty signature":           "",
		"base64 of the wrong length":   base64.RawStdEncoding.EncodeToString([]byte("too short")),
		"characters outside base64":    strings.Repeat("!", 86),
		"standard base64 with padding": base64.StdEncoding.EncodeToString(make([]byte, 64)),
	} {
		tampered := signed[:opening+13] + replacement + signed[opening+13+closing:]
		writeRawAttestation(t, gate, []byte(tampered))
		err := gate.Check(context.Background(), gate.approvalID)
		if err == nil {
			t.Fatalf("%s was accepted", name)
		}
		if !strings.Contains(err.Error(), "signature is malformed") {
			t.Fatalf("%s refused for the wrong reason: %v", name, err)
		}
	}
}

// A sound signature from the wrong key is the case the gate exists for: the
// envelope is well formed and every date is fresh, and it is still refused.
func TestAnAttestationSignedByAnotherKeyFailsVerification(t *testing.T) {
	gate, attestation, _ := backupGateFixture(t)
	_, other, err := ed25519.GenerateKey(nil)
	if err != nil {
		t.Fatal(err)
	}
	writeRawAttestation(t, gate, signRawAttestation(t, attestation, other))
	gateRefusal(t, gate, "signature verification failed")
}

// The signer refuses a request it cannot stand behind, so the monitor cannot
// publish an envelope the gate would have to reason about later.
func TestSignBackupAttestationRefusesARequestItCannotStandBehind(t *testing.T) {
	_, attestation, key := backupGateFixture(t)
	for name, build := range map[string]func() (BackupAttestation, ed25519.PrivateKey){
		"a key of the wrong length": func() (BackupAttestation, ed25519.PrivateKey) {
			return attestation, key[:16]
		},
		"no key at all": func() (BackupAttestation, ed25519.PrivateKey) {
			return attestation, nil
		},
		"another schema": func() (BackupAttestation, ed25519.PrivateKey) {
			widened := attestation
			widened.SchemaVersion = backupAttestationSchema + 1
			return widened, key
		},
		"an approval that is not an identifier": func() (BackupAttestation, ed25519.PrivateKey) {
			renamed := attestation
			renamed.ApprovalID = "backups"
			return renamed, key
		},
		"no check instant": func() (BackupAttestation, ed25519.PrivateKey) {
			undated := attestation
			undated.CheckedAt = time.Time{}
			return undated, key
		},
		"no full backup": func() (BackupAttestation, ed25519.PrivateKey) {
			undated := attestation
			undated.LatestFullBackup = time.Time{}
			return undated, key
		},
		"no restore drill": func() (BackupAttestation, ed25519.PrivateKey) {
			undated := attestation
			undated.RestoreDrillAt = time.Time{}
			return undated, key
		},
		"a signature already on it": func() (BackupAttestation, ed25519.PrivateKey) {
			presigned := attestation
			presigned.Signature = base64.RawStdEncoding.EncodeToString(make([]byte, 64))
			return presigned, key
		},
	} {
		request, signing := build()
		raw, err := SignBackupAttestation(request, signing)
		if err == nil {
			t.Fatalf("%s was signed", name)
		}
		if raw != nil {
			t.Fatalf("%s returned an envelope alongside its refusal", name)
		}
		if !strings.Contains(err.Error(), "invalid backup attestation signing request") {
			t.Fatalf("%s refused for the wrong reason: %v", name, err)
		}
	}
}
