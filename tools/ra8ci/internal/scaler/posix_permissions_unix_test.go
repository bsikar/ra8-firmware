//go:build unix

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package scaler

import (
	context "context"
	ed25519 "crypto/ed25519"
	base64 "encoding/base64"
	os "os"
	filepath "path/filepath"
	strings "strings"
	testing "testing"
	time "time"
)

func TestProtectedPgBackRestDirectoryIsStillAccepted(t *testing.T) {
	config := monitorWithBinaryDirectory(t, 0o750)
	if err := RefreshBackupAttestation(context.Background(), config); err != nil {
		t.Fatalf("protected executable directory was refused: %v", err)
	}
}

func TestOwnerWritablePgBackRestDirectoryIsStillAccepted(t *testing.T) {
	config := monitorWithBinaryDirectory(t, 0o700)
	if err := RefreshBackupAttestation(context.Background(), config); err != nil {
		t.Fatalf("owner-writable executable directory was refused: %v", err)
	}
}

func TestGroupWritablePgBackRestDirectoryIsRefused(t *testing.T) {
	config := monitorWithBinaryDirectory(t, 0o770)
	err := RefreshBackupAttestation(context.Background(), config)
	if err == nil {
		t.Fatal("group-writable pgBackRest directory was accepted")
	}
	if !strings.Contains(err.Error(), "directory must not be group or world writable") {
		t.Fatalf("refused for the wrong reason: %v", err)
	}
}

func TestWorldWritablePgBackRestDirectoryIsRefused(t *testing.T) {
	config := monitorWithBinaryDirectory(t, 0o757)
	err := RefreshBackupAttestation(context.Background(), config)
	if err == nil {
		t.Fatal("world-writable pgBackRest directory was accepted")
	}
	if !strings.Contains(err.Error(), "directory must not be group or world writable") {
		t.Fatalf("refused for the wrong reason: %v", err)
	}
}

// A sticky bit stops another account unlinking a file it does not own, so a
// 1777 directory does block the substitution this rule is about. It is still
// refused: every other monitor input draws the line at group and other write
// with no exception, and a privileged executable does not belong in a shared
// scratch directory. Deleting this test and masking os.ModeSticky is the one
// change if that judgement is ever revisited.
func TestStickyWorldWritablePgBackRestDirectoryIsRefused(t *testing.T) {
	config := monitorWithBinaryDirectory(t, 0o777|os.ModeSticky)
	err := RefreshBackupAttestation(context.Background(), config)
	if err == nil {
		t.Fatal("sticky world-writable pgBackRest directory was accepted")
	}
	if !strings.Contains(err.Error(), "directory must not be group or world writable") {
		t.Fatalf("refused for the wrong reason: %v", err)
	}
}

func TestWritablePgBackRestDirectorySignsNothing(t *testing.T) {
	config := monitorWithBinaryDirectory(t, 0o777)
	if err := RefreshBackupAttestation(context.Background(), config); err == nil {
		t.Fatal("world-writable pgBackRest directory was accepted")
	}
	if _, err := os.Stat(config.AttestationPath); !os.IsNotExist(err) {
		t.Fatal("a refused run published an attestation")
	}
}

// The executable rule keeps its own reason when both are wrong, so an operator
// is told about the file in front of them rather than its parent.
func TestWritableExecutableStillNamesTheExecutable(t *testing.T) {
	config, _ := backupMonitorFixture(t, 0o777)
	err := RefreshBackupAttestation(context.Background(), config)
	if err == nil {
		t.Fatal("world-writable pgBackRest executable was accepted")
	}
	if !strings.Contains(err.Error(), "executable must not be group or world writable") {
		t.Fatalf("the pre-existing rule no longer names its own reason: %v", err)
	}
}

func TestProtectedAttestationDirectoryIsStillAccepted(t *testing.T) {
	config := monitorWithOutputDirectory(t, 0o750)
	if err := RefreshBackupAttestation(context.Background(), config); err != nil {
		t.Fatalf("protected attestation directory was refused: %v", err)
	}
	if _, err := os.Stat(config.AttestationPath); err != nil {
		t.Fatalf("an accepted run published nothing: %v", err)
	}
}

func TestOwnerWritableAttestationDirectoryIsStillAccepted(t *testing.T) {
	config := monitorWithOutputDirectory(t, 0o700)
	if err := RefreshBackupAttestation(context.Background(), config); err != nil {
		t.Fatalf("owner-writable attestation directory was refused: %v", err)
	}
}

// The rule the other four monitor directories already hold: group write is
// permission to unlink and rename the published attestation, whatever the
// file's own 0640 says.
func TestGroupWritableAttestationDirectoryIsRefused(t *testing.T) {
	config := monitorWithOutputDirectory(t, 0o770)
	err := RefreshBackupAttestation(context.Background(), config)
	if err == nil {
		t.Fatal("group-writable attestation directory was accepted")
	}
	if !strings.Contains(err.Error(), "attestation directory must not be group or world writable") {
		t.Fatalf("refused for the wrong reason: %v", err)
	}
	if _, err := os.Stat(config.AttestationPath); !os.IsNotExist(err) {
		t.Fatal("a refused run published an attestation")
	}
}

func TestWorldWritableAttestationDirectoryIsRefused(t *testing.T) {
	config := monitorWithOutputDirectory(t, 0o757)
	err := RefreshBackupAttestation(context.Background(), config)
	if err == nil {
		t.Fatal("world-writable attestation directory was accepted")
	}
	if !strings.Contains(err.Error(), "attestation directory must not be group or world writable") {
		t.Fatalf("refused for the wrong reason: %v", err)
	}
}

// Same call as every other directory in this file: the sticky bit buys no
// exception.
func TestStickyWorldWritableAttestationDirectoryIsRefused(t *testing.T) {
	config := monitorWithOutputDirectory(t, os.ModeSticky|0o777)
	if err := RefreshBackupAttestation(context.Background(), config); err == nil {
		t.Fatal("sticky world-writable attestation directory was accepted")
	}
}

func TestProtectedSigningKeyDirectoryIsStillAccepted(t *testing.T) {
	config := monitorWithKeyDirectory(t, 0o750)
	if err := RefreshBackupAttestation(context.Background(), config); err != nil {
		t.Fatalf("protected signing key directory was refused: %v", err)
	}
}

// The monitor owns the key it signs with, so owning its directory outright is
// the ordinary deployment.
func TestOwnerWritableSigningKeyDirectoryIsStillAccepted(t *testing.T) {
	config := monitorWithKeyDirectory(t, 0o700)
	if err := RefreshBackupAttestation(context.Background(), config); err != nil {
		t.Fatalf("owner-writable signing key directory was refused: %v", err)
	}
}

func TestGroupWritableSigningKeyDirectoryIsRefused(t *testing.T) {
	config := monitorWithKeyDirectory(t, 0o770)
	err := RefreshBackupAttestation(context.Background(), config)
	if err == nil {
		t.Fatal("group-writable signing key directory was accepted")
	}
	if !strings.Contains(err.Error(), "signing key directory must not be group or world writable") {
		t.Fatalf("refused for the wrong reason: %v", err)
	}
}

func TestWorldWritableSigningKeyDirectoryIsRefused(t *testing.T) {
	config := monitorWithKeyDirectory(t, 0o757)
	err := RefreshBackupAttestation(context.Background(), config)
	if err == nil {
		t.Fatal("world-writable signing key directory was accepted")
	}
	if !strings.Contains(err.Error(), "signing key directory must not be group or world writable") {
		t.Fatalf("refused for the wrong reason: %v", err)
	}
}

// Same call as every other monitor input: the sticky bit buys no exception.
func TestStickyWorldWritableSigningKeyDirectoryIsRefused(t *testing.T) {
	config := monitorWithKeyDirectory(t, os.ModeSticky|0o777)
	if err := RefreshBackupAttestation(context.Background(), config); err == nil {
		t.Fatal("sticky world-writable signing key directory was accepted")
	}
}

func TestWritableSigningKeyDirectorySignsNothing(t *testing.T) {
	config := monitorWithKeyDirectory(t, 0o777)
	if err := RefreshBackupAttestation(context.Background(), config); err == nil {
		t.Fatal("world-writable signing key directory was accepted")
	}
	if _, err := os.Stat(config.AttestationPath); !os.IsNotExist(err) {
		t.Fatal("a refused run published an attestation")
	}
}

func TestProtectedDrillReceiptDirectoryIsStillAccepted(t *testing.T) {
	config := monitorWithDrillDirectory(t, 0o750)
	if err := RefreshBackupAttestation(context.Background(), config); err != nil {
		t.Fatalf("protected restore drill receipt directory was refused: %v", err)
	}
}

// The drill runner is a different identity from the monitor, so it owns the
// directory it publishes into and needs no group write to do it.
func TestOwnerWritableDrillReceiptDirectoryIsStillAccepted(t *testing.T) {
	config := monitorWithDrillDirectory(t, 0o700)
	if err := RefreshBackupAttestation(context.Background(), config); err != nil {
		t.Fatalf("owner-writable restore drill receipt directory was refused: %v", err)
	}
}

func TestGroupWritableDrillReceiptDirectoryIsRefused(t *testing.T) {
	config := monitorWithDrillDirectory(t, 0o770)
	err := RefreshBackupAttestation(context.Background(), config)
	if err == nil {
		t.Fatal("group-writable restore drill receipt directory was accepted")
	}
	if !strings.Contains(err.Error(), "receipt directory must not be group or world writable") {
		t.Fatalf("refused for the wrong reason: %v", err)
	}
}

func TestWorldWritableDrillReceiptDirectoryIsRefused(t *testing.T) {
	config := monitorWithDrillDirectory(t, 0o757)
	err := RefreshBackupAttestation(context.Background(), config)
	if err == nil {
		t.Fatal("world-writable restore drill receipt directory was accepted")
	}
	if !strings.Contains(err.Error(), "receipt directory must not be group or world writable") {
		t.Fatalf("refused for the wrong reason: %v", err)
	}
}

// The sticky bit does block this substitution, but every other monitor input
// draws the line at group or other write with no exception, and a receipt the
// whole gate ages does not belong in shared scratch. Pinned so a later change
// is deliberate.
func TestStickyWorldWritableDrillReceiptDirectoryIsRefused(t *testing.T) {
	config := monitorWithDrillDirectory(t, os.ModeSticky|0o777)
	if err := RefreshBackupAttestation(context.Background(), config); err == nil {
		t.Fatal("sticky world-writable restore drill receipt directory was accepted")
	}
}

func TestWritableDrillReceiptDirectorySignsNothing(t *testing.T) {
	config := monitorWithDrillDirectory(t, 0o777)
	if err := RefreshBackupAttestation(context.Background(), config); err == nil {
		t.Fatal("world-writable restore drill receipt directory was accepted")
	}
	if _, err := os.Stat(config.AttestationPath); !os.IsNotExist(err) {
		t.Fatal("a refused run published an attestation")
	}
}

func TestProtectedPublicKeyDirectoryIsStillAccepted(t *testing.T) {
	key, err := LoadBackupPublicKey(publicKeyInDirectory(t, 0o755))
	if err != nil || len(key) != ed25519.PublicKeySize {
		t.Fatalf("protected public key directory was refused: %v", err)
	}
}

// The API service reading a key out of a directory it owns is ordinary.
func TestOwnerWritablePublicKeyDirectoryIsStillAccepted(t *testing.T) {
	key, err := LoadBackupPublicKey(publicKeyInDirectory(t, 0o700))
	if err != nil || len(key) != ed25519.PublicKeySize {
		t.Fatalf("owner-writable public key directory was refused: %v", err)
	}
}

func TestGroupWritablePublicKeyDirectoryIsRefused(t *testing.T) {
	key, err := LoadBackupPublicKey(publicKeyInDirectory(t, 0o770))
	if err == nil {
		t.Fatal("group-writable public key directory was accepted")
	}
	if key != nil {
		t.Fatal("a refused load returned key material")
	}
	if !strings.Contains(err.Error(), "public key directory must not be group or world writable") {
		t.Fatalf("refused for the wrong reason: %v", err)
	}
}

func TestWorldWritablePublicKeyDirectoryIsRefused(t *testing.T) {
	_, err := LoadBackupPublicKey(publicKeyInDirectory(t, 0o757))
	if err == nil {
		t.Fatal("world-writable public key directory was accepted")
	}
	if !strings.Contains(err.Error(), "public key directory must not be group or world writable") {
		t.Fatalf("refused for the wrong reason: %v", err)
	}
}

// Same call as every other input in this file: the sticky bit buys no
// exception, so a future fire that wants one changes it deliberately.
func TestStickyWorldWritablePublicKeyDirectoryIsRefused(t *testing.T) {
	if _, err := LoadBackupPublicKey(publicKeyInDirectory(t, os.ModeSticky|0o777)); err == nil {
		t.Fatal("sticky world-writable public key directory was accepted")
	}
}

func TestProtectedPgBackRestExecutableIsStillAccepted(t *testing.T) {
	config, public := backupMonitorFixture(t, 0o750)
	if err := RefreshBackupAttestation(context.Background(), config); err != nil {
		t.Fatalf("protected executable was refused: %v", err)
	}
	gate, err := NewSignedBackupGate(config.AttestationPath, public, config.ApprovalID, time.Hour, 48*time.Hour, 90*24*time.Hour)
	if err != nil {
		t.Fatal(err)
	}
	if err := gate.Check(context.Background(), config.ApprovalID); err != nil {
		t.Fatalf("signed evidence failed verification: %v", err)
	}
}

func TestOwnerWritablePgBackRestExecutableIsStillAccepted(t *testing.T) {
	config, _ := backupMonitorFixture(t, 0o700)
	if err := RefreshBackupAttestation(context.Background(), config); err != nil {
		t.Fatalf("owner-writable executable was refused: %v", err)
	}
}

func TestGroupWritablePgBackRestExecutableIsRefused(t *testing.T) {
	config, _ := backupMonitorFixture(t, 0o770)
	err := RefreshBackupAttestation(context.Background(), config)
	if err == nil {
		t.Fatal("group-writable pgBackRest executable was accepted")
	}
	if !strings.Contains(err.Error(), "group or world writable") {
		t.Fatalf("refused for the wrong reason: %v", err)
	}
}

func TestWorldWritablePgBackRestExecutableIsRefused(t *testing.T) {
	config, _ := backupMonitorFixture(t, 0o757)
	err := RefreshBackupAttestation(context.Background(), config)
	if err == nil {
		t.Fatal("world-writable pgBackRest executable was accepted")
	}
	if !strings.Contains(err.Error(), "group or world writable") {
		t.Fatalf("refused for the wrong reason: %v", err)
	}
}

func TestWritablePgBackRestExecutableSignsNothing(t *testing.T) {
	config, _ := backupMonitorFixture(t, 0o777)
	if err := RefreshBackupAttestation(context.Background(), config); err == nil {
		t.Fatal("world-writable pgBackRest executable was accepted")
	}
	if _, err := os.Stat(config.AttestationPath); !os.IsNotExist(err) {
		t.Fatal("a refused run published an attestation")
	}
}

func TestNonExecutablePgBackRestIsStillRefusedAsNotExecutable(t *testing.T) {
	config, _ := backupMonitorFixture(t, 0o640)
	err := RefreshBackupAttestation(context.Background(), config)
	if err == nil {
		t.Fatal("non-executable pgBackRest was accepted")
	}
	if !strings.Contains(err.Error(), "executable regular file") {
		t.Fatalf("the pre-existing rule no longer names its own reason: %v", err)
	}
}

// seal makes a file unopenable while leaving the mode the policy checks read
// unchanged, and skips the case when this process can read it anyway.
func seal(t *testing.T, path string) {
	t.Helper()
	if err := os.Chmod(path, 0o000); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = os.Chmod(path, 0o600) })
	if _, err := os.ReadFile(path); err == nil {
		t.Skip("this process can read a sealed file")
	}
}

// A drill receipt whose mode passes policy but which will not open is named as
// unreadable, not as badly permissioned. The two have different fixes.
func TestADrillReceiptThatWillNotOpenIsNamedAsSuch(t *testing.T) {
	config, _ := backupMonitorFixture(t, 0o750)
	seal(t, config.RestoreDrillPath)

	err := RefreshBackupAttestation(context.Background(), config)
	if err == nil || err.Error() != "open restore drill receipt" {
		t.Fatalf("answered %v, want the drill receipt named as unopenable", err)
	}
}

// The same for the signing key, and it is reached only because the drill
// receipt ahead of it was fine: the monitor validates its inputs in order, so
// this also pins that the key is read after the receipt rather than before.
func TestASigningKeyThatWillNotOpenIsNamedAsSuch(t *testing.T) {
	config, _ := backupMonitorFixture(t, 0o750)
	seal(t, config.PrivateKeyPath)

	err := RefreshBackupAttestation(context.Background(), config)
	if err == nil || err.Error() != "open backup signing key" {
		t.Fatalf("answered %v, want the signing key named as unopenable", err)
	}
}

// The gate's own key decides which signatures it will accept, so the loader
// holds it to the same rule and distinguishes the same two failures.
func TestAPublicKeyThatWillNotOpenIsNamedAsSuch(t *testing.T) {
	public, _, err := ed25519.GenerateKey(nil)
	if err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(t.TempDir(), "backup.pub")
	if err := os.WriteFile(path, []byte(base64.StdEncoding.EncodeToString(public)), 0o644); err != nil {
		t.Fatal(err)
	}

	if loaded, err := LoadBackupPublicKey(path); err != nil || !loaded.Equal(public) {
		t.Fatalf("a readable key was refused: %v", err)
	}
	seal(t, path)
	if _, err := LoadBackupPublicKey(path); err == nil || err.Error() != "open backup public key" {
		t.Fatalf("answered %v, want the public key named as unopenable", err)
	}
}

// Both halves are owner-only and decode to a usable ed25519 pair, and the
// second attempt at the same paths refuses rather than rotating the key out
// from under whoever already trusts it.
func TestCreateBackupSigningKeyPairWritesOwnerOnlyHalvesOnce(t *testing.T) {
	root := t.TempDir()
	private := filepath.Join(root, "backup.key")
	public := filepath.Join(root, "backup.pub")
	if err := CreateBackupSigningKeyPair(private, public); err != nil {
		t.Fatal(err)
	}
	for _, path := range []string{private, public} {
		info, err := os.Lstat(path)
		if err != nil {
			t.Fatal(err)
		}
		if info.Mode().Perm() != 0o600 {
			t.Fatalf("%s mode = %v", filepath.Base(path), info.Mode().Perm())
		}
		raw, err := os.ReadFile(path)
		if err != nil {
			t.Fatal(err)
		}
		if _, err := base64.StdEncoding.DecodeString(strings.TrimSpace(string(raw))); err != nil {
			t.Fatalf("%s is not base64: %v", filepath.Base(path), err)
		}
	}
	if err := CreateBackupSigningKeyPair(private, public); err == nil {
		t.Fatal("an existing signing key was replaced")
	}
}

func TestAPublishedAttestationLandsOwnerReadableWithNoTemporaryLeftBehind(t *testing.T) {
	config, _ := backupMonitorFixture(t, 0o750)
	if err := RefreshBackupAttestation(context.Background(), config); err != nil {
		t.Fatalf("RefreshBackupAttestation: %v", err)
	}
	info, err := os.Lstat(config.AttestationPath)
	if err != nil {
		t.Fatalf("an accepted run published nothing: %v", err)
	}
	if !info.Mode().IsRegular() {
		t.Fatalf("published attestation is not a regular file: %v", info.Mode())
	}
	// 0640 is the published mode: the API service reads it as a group member,
	// and nobody outside the group reads it at all.
	if info.Mode().Perm() != 0o640 {
		t.Fatalf("published attestation mode is %v, want 0640", info.Mode().Perm())
	}
	if info.Size() == 0 {
		t.Fatal("published attestation is empty")
	}
	if stray := attestationLeftovers(t, filepath.Dir(config.AttestationPath)); len(stray) != 0 {
		t.Fatalf("publication left a temporary behind: %v", stray)
	}
}

// sealDir makes a directory unwritable while leaving the permission bits the
// policy reads acceptable, and skips the case when this process can write it
// anyway.
func sealDir(t *testing.T, path string) {
	t.Helper()
	if err := os.Chmod(path, 0o500); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = os.Chmod(path, 0o700) })
	probe := filepath.Join(path, ".probe")
	if err := os.WriteFile(probe, []byte("x"), 0o600); err == nil {
		_ = os.Remove(probe)
		t.Skip("this process can write a sealed directory")
	}
}

// A directory nobody else can write can still refuse this process, and that
// is reported as a creation failure rather than as a policy refusal.
func TestAKeyThatCannotBeCreatedIsNamedAsSuch(t *testing.T) {
	root := t.TempDir()
	sealDir(t, root)

	err := CreateBackupSigningKeyPair(filepath.Join(root, "signing.key"), filepath.Join(root, "signing.pub"))
	if err == nil || err.Error() != "create backup key file without replacement" {
		t.Fatalf("answered %v, want the creation failure named", err)
	}
}

// When the public half cannot be written the private half is taken back, so a
// failed generation never leaves a lone private key at a path the next
// attempt would then refuse to replace.
func TestAPrivateKeyIsTakenBackWhenThePublicHalfFails(t *testing.T) {
	root := t.TempDir()
	keys := filepath.Join(root, "keys")
	published := filepath.Join(root, "published")
	for _, directory := range []string{keys, published} {
		if err := os.Mkdir(directory, 0o700); err != nil {
			t.Fatal(err)
		}
	}
	private := filepath.Join(keys, "signing.key")
	sealDir(t, published)

	if err := CreateBackupSigningKeyPair(private, filepath.Join(published, "signing.pub")); err == nil {
		t.Fatal("the pair was reported created")
	}
	if _, err := os.Lstat(private); !os.IsNotExist(err) {
		t.Fatalf("the private half was left behind: %v", err)
	}
}

// An attestation whose mode passes the gate's policy check can still refuse to
// open, and that is named as an unreadable file rather than a permissions
// fault. An operator told the file is writable would go and change a mode that
// was never the problem.
func TestAnAttestationThatWillNotOpenIsNamedAsSuch(t *testing.T) {
	gate, attestation, key := backupGateFixture(t)
	writeSignedBackupFixture(t, gate.path, attestation, key)
	if err := gate.Check(context.Background(), gate.approvalID); err != nil {
		t.Fatalf("a readable attestation was refused: %v", err)
	}
	seal(t, gate.path)

	err := gate.Check(context.Background(), gate.approvalID)
	if err == nil || err.Error() != "open signed backup attestation" {
		t.Fatalf("answered %v, want the attestation named as unopenable", err)
	}
}
