//go:build unix

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package provision

import (
	bytes "bytes"
	store "github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
	http "net/http"
	os "os"
	filepath "path/filepath"
	testing "testing"
)

func TestHTTPBackendEnvironmentRejectsUnapprovedEndpointsAndWeakKeys(t *testing.T) {
	base := testHTTPBackendConfig(t)
	for _, endpoint := range []string{
		"http://ra8ci.internal.example:8443",
		"https://ra8ci.internal.example",
		"https://user@ra8ci.internal.example:8443",
		"https://ra8ci.internal.example:8443?x=y",
		"https://ra8ci.internal.example:8443/../outside",
		"https://control.tail123.ts.net:8443",
		"https://100.64.1.2:8443",
	} {
		t.Run(endpoint, func(t *testing.T) {
			config := base
			config.ServerURL = endpoint
			if _, err := HTTPBackendEnvironment(config); err == nil {
				t.Fatalf("accepted unapproved endpoint %q", endpoint)
			}
		})
	}
	if err := os.Chmod(base.ClientPrivateKeyFile, 0o640); err != nil {
		t.Fatal(err)
	}
	if _, err := HTTPBackendEnvironment(base); err == nil {
		t.Fatal("accepted a private key accessible to group")
	}
}

func TestSSHAccessStoreRejectsUnsafeKeyPermissions(t *testing.T) {
	keys, err := NewSSHAccessStore(filepath.Join(t.TempDir(), "keys"))
	if err != nil {
		t.Fatal(err)
	}
	reservationID, err := store.NewID()
	if err != nil {
		t.Fatal(err)
	}
	key, err := keys.Ensure(reservationID)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.Chmod(key.PrivateKeyFile, 0o644); err != nil {
		t.Fatal(err)
	}
	if _, err := keys.Ensure(reservationID); err == nil {
		t.Fatal("accepted a group-readable SSH private key")
	}
	if err := keys.Remove(reservationID); err == nil {
		t.Fatal("removed a key that violates private-file policy")
	}
}

func TestTerraformWorkspacePathAndPrivateFileFence(t *testing.T) {
	root := t.TempDir()
	workspace := filepath.Join(root, "reservation")
	if err := os.Mkdir(workspace, 0o700); err != nil {
		t.Fatal(err)
	}
	privateFile := filepath.Join(workspace, "variables.tfvars.json")
	if err := os.WriteFile(privateFile, []byte("{\"runner\":{}}"), 0o600); err != nil {
		t.Fatal(err)
	}
	if !pathInside(workspace, privateFile) || pathInside(workspace, root) ||
		pathInside(workspace, workspace) {
		t.Fatal("workspace path containment returned an unexpected result")
	}
	if err := requirePrivateFileInside(workspace, privateFile, 1024); err != nil {
		t.Fatalf("accepted private workspace file: %v", err)
	}
	if err := requirePrivateFileInside(workspace, filepath.Join(root, "outside"), 1024); err == nil {
		t.Fatal("accepted a variable file outside the reservation workspace")
	}
	if err := os.Chmod(privateFile, 0o644); err != nil {
		t.Fatal(err)
	}
	if err := requirePrivateFileInside(workspace, privateFile, 1024); err == nil {
		t.Fatal("accepted a variable file readable by other users")
	}
}

// TestACredentialFileThePolicyAdmitsButCannotBeOpened pins the gap between the
// file policy and the read. A mode 0o000 file is a regular file, inside the
// size bound, with no group or other bits, so it passes every policy check and
// still cannot be opened. Each of the three credentials keeps its own wrapper,
// which is what tells an operator WHICH file to look at.
func TestACredentialFileThePolicyAdmitsButCannotBeOpened(t *testing.T) {
	for name, pick := range map[string]func(AppRoleConfig) (string, string){
		"role ID":   func(c AppRoleConfig) (string, string) { return c.RoleIDFile, "read AppRole role ID" },
		"secret ID": func(c AppRoleConfig) (string, string) { return c.SecretIDFile, "read AppRole secret ID" },
		"CA bundle": func(c AppRoleConfig) (string, string) { return c.CAFile, "read AppRole CA bundle" },
	} {
		t.Run(name, func(t *testing.T) {
			server, authority := appRoleAnswering(t, http.StatusOK, appRoleGoodAnswer)
			defer server.Close()
			config := appRoleConfigWithCA(t, server.URL+"/bao", authority)
			file, wrapper := pick(config)
			if err := os.Chmod(file, 0o000); err != nil {
				t.Fatalf("seal credential file: %v", err)
			}
			appRoleRefused(t, name, config, wrapper)
			appRoleRefused(t, name, config, "credential file is unreadable")
		})
	}
}

// TestAnOpenedDirectoryIsTightenedAndCleaned pins that opening a store fixes
// the directory it was handed rather than trusting how it was found.
func TestAnOpenedDirectoryIsTightenedAndCleaned(t *testing.T) {
	root := filepath.Join(t.TempDir(), "keys")
	if err := os.MkdirAll(root, 0o777); err != nil {
		t.Fatal(err)
	}
	if err := os.Chmod(root, 0o777); err != nil {
		t.Fatal(err)
	}
	keys, err := NewSSHAccessStore(root + string(filepath.Separator) + "." + string(filepath.Separator))
	if err != nil {
		t.Fatalf("open store over an existing directory: %v", err)
	}
	info, err := os.Stat(root)
	if err != nil || info.Mode().Perm() != 0o700 {
		t.Fatalf("world-readable directory was not tightened to 0700: %v, %v", info, err)
	}
	reservationID := reservationKeyID(t)
	key, err := keys.Ensure(reservationID)
	if err != nil {
		t.Fatal(err)
	}
	if key.PrivateKeyFile != filepath.Join(root, reservationID+".key") {
		t.Fatalf("uncleaned directory leaked into the key path: %q", key.PrivateKeyFile)
	}
}

// TestAnUnsafeKeyIsLeftWhereItIs pins that a refused removal changes nothing,
// so the evidence of how the file got that way survives for whoever looks.
func TestAnUnsafeKeyIsLeftWhereItIs(t *testing.T) {
	keys, root := reservationKeyStore(t)
	reservationID := reservationKeyID(t)
	key, err := keys.Ensure(reservationID)
	if err != nil {
		t.Fatal(err)
	}
	before, err := os.ReadFile(key.PrivateKeyFile)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.Chmod(key.PrivateKeyFile, 0o644); err != nil {
		t.Fatal(err)
	}
	if err := keys.Remove(reservationID); err == nil ||
		err.Error() != "refusing to remove an unsafe SSH key file" {
		t.Fatalf("an unsafe key was removed or misreported: %v", err)
	}
	after, err := os.ReadFile(key.PrivateKeyFile)
	if err != nil || !bytes.Equal(before, after) {
		t.Fatalf("a refused removal disturbed the file: %v", err)
	}
	if err := os.Chmod(key.PrivateKeyFile, 0o600); err != nil {
		t.Fatal(err)
	}
	if err := keys.Remove(reservationID); err != nil {
		t.Fatalf("a repaired key could not be removed: %v", err)
	}
	if names := rootEntries(t, root); len(names) != 0 {
		t.Fatalf("the directory still holds something: %v", names)
	}
}

// TestThePrivateKeyMayNotBeReadableByAnyoneElse pins the one rule that is
// asked of the key file and of nothing else. Group and other are both
// refused, and the mode is read from the file rather than from the handle's
// owner, so a key left world-readable on the service disk never reaches a
// Terraform child.
func TestThePrivateKeyMayNotBeReadableByAnyoneElse(t *testing.T) {
	for _, mode := range []os.FileMode{0o640, 0o604, 0o660, 0o606, 0o644} {
		t.Run(mode.String(), func(t *testing.T) {
			config := shapedBackendConfig(t, nil)
			if err := os.Chmod(config.ClientPrivateKeyFile, mode); err != nil {
				t.Fatal(err)
			}
			err := refusedBackend(t, config)
			mustContain(t, err, "read Terraform client private key")
			mustContain(t, err, "private key file must not be accessible by group or others")
		})
	}
	config := shapedBackendConfig(t, nil)
	if err := os.Chmod(config.ClientPrivateKeyFile, 0o400); err != nil {
		t.Fatal(err)
	}
	if _, err := HTTPBackendEnvironment(config); err != nil {
		t.Fatalf("refused an owner-only private key: %v", err)
	}
}

func TestCredentialCertificateThatCannotBeOpenedIsRefused(t *testing.T) {
	config := shapedBackendConfig(t, nil)
	if err := os.Chmod(config.ClientCertificateFile, 0o000); err != nil {
		t.Fatal(err)
	}
	mustContain(t, refusedBackend(t, config), "credential file is unreadable")
}

func TestAppRoleCredentialsMustNotBeGroupReadable(t *testing.T) {
	server, authority := appRoleAnswering(t, http.StatusOK, appRoleGoodAnswer)
	defer server.Close()
	for _, item := range []struct {
		name string
		path func(AppRoleConfig) string
		want string
	}{
		{"role ID", func(c AppRoleConfig) string { return c.RoleIDFile }, "read AppRole role ID"},
		{"secret ID", func(c AppRoleConfig) string { return c.SecretIDFile }, "read AppRole secret ID"},
	} {
		t.Run(item.name, func(t *testing.T) {
			config := appRoleConfigWithCA(t, server.URL+"/bao", authority)
			if err := os.Chmod(item.path(config), 0o640); err != nil {
				t.Fatal(err)
			}
			appRoleRefused(t, item.name, config, item.want)
		})
	}
}

// sealedRoot makes the store directory unwritable for the rest of the test and
// restores it before the temporary directory is removed.
func sealedRoot(t *testing.T, root string) {
	t.Helper()
	if err := os.Chmod(root, 0o500); err != nil {
		t.Fatalf("seal store directory: %v", err)
	}
	t.Cleanup(func() { _ = os.Chmod(root, 0o700) })
}

// TestADirectoryThatWillNotTakeANewKey pins the refusal a mint meets when the
// service disk has gone read-only under it. The reservation is left with no
// key at all, which is the honest outcome: a half-written key would be worse
// than none, because a guest booted against it can never be reached again.
func TestADirectoryThatWillNotTakeANewKey(t *testing.T) {
	keys, root := reservationKeyStore(t)
	reservationID := reservationKeyID(t)
	sealedRoot(t, root)

	key, err := keys.Ensure(reservationID)
	if err == nil {
		t.Fatal("Ensure minted a key into a directory it cannot write")
	}
	if err.Error() != "create temporary reservation SSH key" {
		t.Fatalf("mint refusal reads %q", err.Error())
	}
	if key.PublicKey != "" || key.PrivateKeyFile != "" {
		t.Fatalf("refused mint still described a key: %+v", key)
	}
	if entries := rootEntries(t, root); len(entries) != 0 {
		t.Fatalf("refused mint left %v behind", entries)
	}
}

// TestAKeyThatCannotBeUnlinked pins that Remove reports the failure rather
// than reporting success over a key that is still on disk. A caller that
// believes a key is gone stops guarding it.
func TestAKeyThatCannotBeUnlinked(t *testing.T) {
	keys, root := reservationKeyStore(t)
	reservationID := reservationKeyID(t)
	file := planted(t, root, reservationID, 0o600, []byte("not a key, never read\n"))
	sealedRoot(t, root)

	err := keys.Remove(reservationID)
	if err == nil {
		t.Fatal("Remove reported success over a key it could not unlink")
	}
	if err.Error() != "remove reservation SSH private key" {
		t.Fatalf("remove refusal reads %q", err.Error())
	}
	if _, statErr := os.Lstat(file); statErr != nil {
		t.Fatalf("key file went missing after a refused remove: %v", statErr)
	}
}

// TestAKeyThePolicyAdmitsButTheDiskWillNotOpen pins the gap between the
// private-file policy and the read itself. A mode 0o000 file satisfies every
// policy check (regular, in range, no group or other bits) and still cannot be
// opened, so the refusal has to come from the open and not from the policy.
func TestAKeyThePolicyAdmitsButTheDiskWillNotOpen(t *testing.T) {
	keys, root := reservationKeyStore(t)
	reservationID := reservationKeyID(t)
	file := planted(t, root, reservationID, 0o000, []byte("sealed\n"))

	if _, err := keys.Load(reservationID); err == nil {
		t.Fatal("Load read a key file it has no permission to open")
	} else if err.Error() != "read reservation SSH private key" {
		t.Fatalf("load refusal reads %q", err.Error())
	}

	// The same file must stop Ensure too, and stop it BEFORE it mints:
	// an unreadable key is a key that may still be installed on a live
	// guest, so quietly replacing it would lock us out of that guest.
	if _, err := keys.Ensure(reservationID); err == nil {
		t.Fatal("Ensure minted over a key it could not read")
	} else if err.Error() != "read reservation SSH private key" {
		t.Fatalf("ensure refusal reads %q", err.Error())
	}
	if entries := rootEntries(t, root); len(entries) != 1 || entries[0] != filepath.Base(file) {
		t.Fatalf("store directory holds %v after two refusals", entries)
	}
	info, err := os.Lstat(file)
	if err != nil {
		t.Fatalf("stat sealed key: %v", err)
	}
	if info.Mode().Perm() != 0o000 || info.Size() != int64(len("sealed\n")) {
		t.Fatalf("sealed key changed under a refusal: mode %v size %d",
			info.Mode().Perm(), info.Size())
	}
}
