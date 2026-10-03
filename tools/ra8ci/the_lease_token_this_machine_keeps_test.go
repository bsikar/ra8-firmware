//go:build linux

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"context"
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/boardclient"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// soundLeaseToken is a token that writeBoardLeaseToken accepts, so a test can
// spoil exactly one field and watch the refusal name that field's shape.
func soundLeaseToken(t *testing.T) boardclient.LeaseToken {
	t.Helper()
	requestID, err := store.NewID()
	if err != nil {
		t.Fatal(err)
	}
	leaseID, err := store.NewID()
	if err != nil {
		t.Fatal(err)
	}
	return boardclient.LeaseToken{BoardID: "ek-ra8d2", RequestID: requestID, LeaseID: leaseID,
		Generation: 3, Version: 8, ExpiresAt: time.Now().UTC().Add(time.Hour).Truncate(time.Second)}
}

// plantLeaseDocument writes a lease document byte for byte, so a test can hand
// the reader something writeBoardLeaseToken would never have produced.
func plantLeaseDocument(t *testing.T, boardID, document string) string {
	t.Helper()
	directory := filepath.Join(t.TempDir(), "leases")
	if err := os.MkdirAll(directory, 0o700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(directory, boardID+".json"), []byte(document), 0o600); err != nil {
		t.Fatal(err)
	}
	return directory
}

func TestBoardLeaseDirectorySitsPrivatelyUnderTheConfigurationHome(t *testing.T) {
	home := t.TempDir()
	t.Setenv("XDG_CONFIG_HOME", home)
	directory, err := currentBoardLeaseDirectory()
	if err != nil {
		t.Fatal(err)
	}
	if want := filepath.Join(home, "ra8ci", "board-leases"); directory != want {
		t.Fatalf("lease directory=%q; want %q", directory, want)
	}
	// The returned path is where leases go; the application directory above it
	// is the one that had to be created, and it had to be created private.
	info, err := os.Lstat(filepath.Join(home, "ra8ci"))
	if err != nil {
		t.Fatal(err)
	}
	if !info.IsDir() || info.Mode().Perm() != 0o700 {
		t.Fatalf("ra8ci configuration directory mode=%v isDir=%v; want a 0700 directory",
			info.Mode().Perm(), info.IsDir())
	}
}

func TestBoardLeaseDirectoryRefusesAConfigurationHomeItCannotTrust(t *testing.T) {
	t.Run("no configuration home at all", func(t *testing.T) {
		t.Setenv("XDG_CONFIG_HOME", "")
		t.Setenv("HOME", "")
		if _, err := currentBoardLeaseDirectory(); err == nil ||
			!strings.Contains(err.Error(), "user configuration directory is unavailable") {
			t.Fatalf("err=%v; want the configuration directory to be reported unavailable", err)
		}
	})
	t.Run("a file where the application directory belongs", func(t *testing.T) {
		home := t.TempDir()
		if err := os.WriteFile(filepath.Join(home, "ra8ci"), []byte("not a directory"), 0o600); err != nil {
			t.Fatal(err)
		}
		t.Setenv("XDG_CONFIG_HOME", home)
		if _, err := currentBoardLeaseDirectory(); err == nil ||
			!strings.Contains(err.Error(), "create private ra8ci configuration directory") {
			t.Fatalf("err=%v; want the creation of the configuration directory to be refused", err)
		}
	})
	t.Run("an application directory the group can read", func(t *testing.T) {
		home := t.TempDir()
		if err := os.MkdirAll(filepath.Join(home, "ra8ci"), 0o755); err != nil {
			t.Fatal(err)
		}
		t.Setenv("XDG_CONFIG_HOME", home)
		if _, err := currentBoardLeaseDirectory(); err == nil ||
			!strings.Contains(err.Error(), "must be private and not a symlink") {
			t.Fatalf("err=%v; want a group-readable configuration directory refused", err)
		}
	})
	t.Run("a symlink standing in for the application directory", func(t *testing.T) {
		home := t.TempDir()
		elsewhere := filepath.Join(t.TempDir(), "elsewhere")
		if err := os.MkdirAll(elsewhere, 0o700); err != nil {
			t.Fatal(err)
		}
		if err := os.Symlink(elsewhere, filepath.Join(home, "ra8ci")); err != nil {
			t.Skipf("symlinks unavailable: %v", err)
		}
		t.Setenv("XDG_CONFIG_HOME", home)
		// MkdirAll is content with a symlink to a directory. Lstat is not, and
		// it is Lstat that decides, so a lease never lands somewhere a link points.
		if _, err := currentBoardLeaseDirectory(); err == nil ||
			!strings.Contains(err.Error(), "must be private and not a symlink") {
			t.Fatalf("err=%v; want a symlinked configuration directory refused", err)
		}
	})
}

func TestWriteBoardLeaseTokenRefusesATokenItCouldNotBindBack(t *testing.T) {
	sound := soundLeaseToken(t)
	spoiled := map[string]func(boardclient.LeaseToken) boardclient.LeaseToken{
		"no board": func(token boardclient.LeaseToken) boardclient.LeaseToken {
			token.BoardID = ""
			return token
		},
		"a board name that is not one": func(token boardclient.LeaseToken) boardclient.LeaseToken {
			token.BoardID = "ek ra8d2/../etc"
			return token
		},
		"a request that is not an identifier": func(token boardclient.LeaseToken) boardclient.LeaseToken {
			token.RequestID = "request-1"
			return token
		},
		"a lease that is not an identifier": func(token boardclient.LeaseToken) boardclient.LeaseToken {
			token.LeaseID = ""
			return token
		},
		"no generation": func(token boardclient.LeaseToken) boardclient.LeaseToken {
			token.Generation = 0
			return token
		},
		"no version": func(token boardclient.LeaseToken) boardclient.LeaseToken {
			token.Version = 0
			return token
		},
		"no deadline": func(token boardclient.LeaseToken) boardclient.LeaseToken {
			token.ExpiresAt = time.Time{}
			return token
		},
	}
	for name, spoil := range spoiled {
		t.Run(name, func(t *testing.T) {
			directory := filepath.Join(t.TempDir(), "leases")
			if err := writeBoardLeaseToken(directory, spoil(sound)); err == nil ||
				!strings.Contains(err.Error(), "invalid board lease token") {
				t.Fatalf("err=%v; want the token refused as invalid", err)
			}
			// A refused token must not even leave a directory behind: the
			// refusal happens before anything is created.
			if _, err := os.Lstat(directory); err == nil {
				t.Fatal("a refused token created the lease directory")
			}
		})
	}
}

func TestWriteBoardLeaseTokenRefusesADirectoryItCannotTrust(t *testing.T) {
	token := soundLeaseToken(t)
	t.Run("a file where the lease directory belongs", func(t *testing.T) {
		parent := t.TempDir()
		if err := os.WriteFile(filepath.Join(parent, "leases"), []byte("not a directory"), 0o600); err != nil {
			t.Fatal(err)
		}
		if err := writeBoardLeaseToken(filepath.Join(parent, "leases"), token); err == nil ||
			!strings.Contains(err.Error(), "create private board lease directory") {
			t.Fatalf("err=%v; want the creation of the lease directory refused", err)
		}
	})
	t.Run("a lease directory the group can read", func(t *testing.T) {
		directory := filepath.Join(t.TempDir(), "leases")
		if err := os.MkdirAll(directory, 0o755); err != nil {
			t.Fatal(err)
		}
		if err := writeBoardLeaseToken(directory, token); err == nil ||
			!strings.Contains(err.Error(), "must be a private, nonsymlink directory") {
			t.Fatalf("err=%v; want a group-readable lease directory refused", err)
		}
	})
	t.Run("a symlink standing in for the lease directory", func(t *testing.T) {
		elsewhere := filepath.Join(t.TempDir(), "elsewhere")
		if err := os.MkdirAll(elsewhere, 0o700); err != nil {
			t.Fatal(err)
		}
		link := filepath.Join(t.TempDir(), "leases")
		if err := os.Symlink(elsewhere, link); err != nil {
			t.Skipf("symlinks unavailable: %v", err)
		}
		if err := writeBoardLeaseToken(link, token); err == nil ||
			!strings.Contains(err.Error(), "must be a private, nonsymlink directory") {
			t.Fatalf("err=%v; want a symlinked lease directory refused", err)
		}
		if entries, err := os.ReadDir(elsewhere); err != nil || len(entries) != 0 {
			t.Fatalf("entries=%d err=%v; want nothing written through the link", len(entries), err)
		}
	})
}

func TestWriteBoardLeaseTokenLeavesNoTemporaryBehindWhenItCannotStore(t *testing.T) {
	token := soundLeaseToken(t)
	directory := filepath.Join(t.TempDir(), "leases")
	if err := os.MkdirAll(filepath.Join(directory, token.BoardID+".json"), 0o700); err != nil {
		t.Fatal(err)
	}
	if err := writeBoardLeaseToken(directory, token); err == nil ||
		!strings.Contains(err.Error(), "atomically store board lease token") {
		t.Fatalf("err=%v; want the atomic store refused", err)
	}
	// The temporary is removed on the way out, so a machine that fails to
	// store a lease does not accumulate half-written tokens beside the real one.
	entries, err := os.ReadDir(directory)
	if err != nil {
		t.Fatal(err)
	}
	for _, entry := range entries {
		if strings.HasPrefix(entry.Name(), ".lease-") {
			t.Fatalf("temporary %q survived a failed store", entry.Name())
		}
	}
}

func TestReadBoardLeaseTokenRefusesABoardNameThatIsNotOne(t *testing.T) {
	directory := plantLeaseDocument(t, "ek-ra8d2", "{}")
	for _, boardID := range []string{"", "ek ra8d2", "../ek-ra8d2", "ek-ra8d2/lease", strings.Repeat("b", 129)} {
		if _, err := readBoardLeaseToken(directory, boardID); err == nil ||
			!strings.Contains(err.Error(), "invalid board ID") {
			t.Fatalf("boardID=%q err=%v; want the board name refused before any file is opened", boardID, err)
		}
	}
}

func TestReadBoardLeaseTokenRefusesADocumentItCannotBelieve(t *testing.T) {
	sound := soundLeaseToken(t)
	encoded, err := json.Marshal(sound)
	if err != nil {
		t.Fatal(err)
	}
	var fields map[string]any
	if err := json.Unmarshal(encoded, &fields); err != nil {
		t.Fatal(err)
	}
	reshape := func(t *testing.T, change func(map[string]any)) string {
		t.Helper()
		copied := make(map[string]any, len(fields)+1)
		for name, value := range fields {
			copied[name] = value
		}
		change(copied)
		document, err := json.Marshal(copied)
		if err != nil {
			t.Fatal(err)
		}
		return string(document)
	}

	t.Run("a document that is not JSON", func(t *testing.T) {
		directory := plantLeaseDocument(t, sound.BoardID, "{\"BoardID\":")
		if _, err := readBoardLeaseToken(directory, sound.BoardID); err == nil ||
			!strings.Contains(err.Error(), "decode board lease token") {
			t.Fatalf("err=%v; want the decode refused", err)
		}
	})
	t.Run("a field this token does not have", func(t *testing.T) {
		document := reshape(t, func(copied map[string]any) { copied["Holder"] = "someone else" })
		directory := plantLeaseDocument(t, sound.BoardID, document)
		if _, err := readBoardLeaseToken(directory, sound.BoardID); err == nil ||
			!strings.Contains(err.Error(), "decode board lease token") {
			t.Fatalf("err=%v; want an unknown field refused rather than dropped", err)
		}
	})
	t.Run("a second document after the first", func(t *testing.T) {
		directory := plantLeaseDocument(t, sound.BoardID, string(encoded)+string(encoded))
		if _, err := readBoardLeaseToken(directory, sound.BoardID); err == nil ||
			!strings.Contains(err.Error(), "trailing data") {
			t.Fatalf("err=%v; want trailing data refused", err)
		}
	})
	t.Run("a token for another board", func(t *testing.T) {
		document := reshape(t, func(copied map[string]any) { copied["BoardID"] = "ek-ra8m1" })
		directory := plantLeaseDocument(t, sound.BoardID, document)
		if _, err := readBoardLeaseToken(directory, sound.BoardID); err == nil ||
			!strings.Contains(err.Error(), "does not match its board") {
			t.Fatalf("err=%v; want a token naming another board refused", err)
		}
	})
	t.Run("a token missing what binds it", func(t *testing.T) {
		for name, change := range map[string]func(map[string]any){
			"request":    func(copied map[string]any) { copied["RequestID"] = "request-1" },
			"lease":      func(copied map[string]any) { copied["LeaseID"] = "" },
			"generation": func(copied map[string]any) { copied["Generation"] = 0 },
			"version":    func(copied map[string]any) { copied["Version"] = 0 },
			"deadline":   func(copied map[string]any) { copied["ExpiresAt"] = time.Time{} },
		} {
			directory := plantLeaseDocument(t, sound.BoardID, reshape(t, change))
			if _, err := readBoardLeaseToken(directory, sound.BoardID); err == nil ||
				!strings.Contains(err.Error(), "does not match its board") {
				t.Fatalf("%s: err=%v; want an unbound token refused", name, err)
			}
		}
	})
}

func TestCheckpointBoardLeaseRefusesBeforeItAsksAnything(t *testing.T) {
	directory, token := heldBoardLease(t)
	client := &fakeBoardLeaseCheckpointer{result: board.Snapshot{BoardID: token.BoardID,
		Phase: board.Draining, Version: token.Version + 1,
		Lease: &board.Lease{ID: token.LeaseID, WaiterID: token.RequestID, Generation: token.Generation}}}

	if _, err := checkpointBoardLease(nil, client, directory, token.BoardID); err == nil ||
		!strings.Contains(err.Error(), "requires context and client") {
		t.Fatalf("err=%v; want a checkpoint without a context refused", err)
	}
	if _, err := checkpointBoardLease(context.Background(), nil, directory, token.BoardID); err == nil ||
		!strings.Contains(err.Error(), "requires context and client") {
		t.Fatalf("err=%v; want a checkpoint without a client refused", err)
	}
	if client.gotToken != (boardclient.LeaseToken{}) {
		t.Fatal("an incomplete checkpoint reached the client")
	}
	// No saved token means nothing to check point against, and the local
	// refusal is handed back as it stands rather than being asked about.
	if _, err := checkpointBoardLease(context.Background(), client, filepath.Join(t.TempDir(), "empty"),
		token.BoardID); err == nil || !strings.Contains(err.Error(), "board lease directory is unavailable") {
		t.Fatalf("err=%v; want the missing lease directory reported", err)
	}
	if client.gotToken != (boardclient.LeaseToken{}) {
		t.Fatal("a checkpoint was asked for without a saved token")
	}
}

func TestExtendBoardLeaseReportsAnExtensionItCouldNotWriteDown(t *testing.T) {
	directory, token := heldBoardLease(t)
	later := token.ExpiresAt.Add(time.Hour)
	// The server answers about the right lease but names version zero, which
	// is not a token this machine can bind back, so the extension is real and
	// the local record is not. That distinction has to reach the operator.
	client := &fakeBoardLeaseExtender{result: board.Snapshot{BoardID: token.BoardID, Version: 0,
		Lease: &board.Lease{ID: token.LeaseID, WaiterID: token.RequestID,
			Generation: token.Generation, ExpiresAt: later}}}
	_, err := extendBoardLease(context.Background(), client, directory, token.BoardID, later, "bring-up run")
	if err == nil || !strings.Contains(err.Error(), "lease was extended but local token could not be updated") {
		t.Fatalf("err=%v; want the extension reported as unrecorded", err)
	}
	if !strings.Contains(err.Error(), "invalid board lease token") {
		t.Fatalf("err=%v; want the write refusal carried along inside it", err)
	}
	// The token on disk still says what it said, so a later beat is made
	// against the deadline this machine can actually prove it holds.
	saved, readErr := readBoardLeaseToken(directory, token.BoardID)
	if readErr != nil || saved != token {
		t.Fatalf("saved=%+v err=%v; want the previous token untouched", saved, readErr)
	}
}

func TestExtendBoardLeaseRefusesBeforeItAsksAnything(t *testing.T) {
	directory, token := heldBoardLease(t)
	client := &fakeBoardLeaseExtender{err: errors.New("server should not have been asked")}
	if _, err := extendBoardLease(nil, client, directory, token.BoardID,
		token.ExpiresAt, "why"); err == nil || !strings.Contains(err.Error(), "requires context and client") {
		t.Fatalf("err=%v; want an extension without a context refused", err)
	}
	if _, err := extendBoardLease(context.Background(), nil, directory, token.BoardID,
		token.ExpiresAt, "why"); err == nil || !strings.Contains(err.Error(), "requires context and client") {
		t.Fatalf("err=%v; want an extension without a client refused", err)
	}
	if _, err := extendBoardLease(context.Background(), client, filepath.Join(t.TempDir(), "empty"),
		token.BoardID, token.ExpiresAt, "why"); err == nil ||
		!strings.Contains(err.Error(), "board lease directory is unavailable") {
		t.Fatalf("err=%v; want the missing lease directory reported", err)
	}
	if client.gotWhy != "" {
		t.Fatal("an extension was asked for without a saved token")
	}
}

func TestHeartbeatBoardLeaseReportsABeatItCouldNotWriteDown(t *testing.T) {
	directory, token := heldBoardLease(t)
	client := beatingServer(token, 0, token.ExpiresAt)
	_, _, err := heartbeatBoardLease(context.Background(), client, directory, token.BoardID)
	if err == nil || !strings.Contains(err.Error(), "holder was reported alive but the local token could not be updated") {
		t.Fatalf("err=%v; want the beat reported as unrecorded", err)
	}
	if client.beats != 1 {
		t.Fatalf("beats=%d; want the beat to have been sent once", client.beats)
	}
	saved, readErr := readBoardLeaseToken(directory, token.BoardID)
	if readErr != nil || saved != token {
		t.Fatalf("saved=%+v err=%v; want the previous token untouched", saved, readErr)
	}
}

func TestHeartbeatBoardLeaseHandsBackWhatTheServerRefused(t *testing.T) {
	directory, token := heldBoardLease(t)
	client := beatingServer(token, token.Version+1, token.ExpiresAt)
	client.err = errors.New("board plane is unreachable")
	if _, _, err := heartbeatBoardLease(context.Background(), client, directory,
		token.BoardID); err == nil || !strings.Contains(err.Error(), "board plane is unreachable") {
		t.Fatalf("err=%v; want the server's own refusal handed back", err)
	}
}

func TestBoardLivenessLineCarriesEveryInstantInUTC(t *testing.T) {
	zone := time.FixedZone("UTC+9", 9*60*60)
	seen := time.Date(2026, 9, 29, 18, 30, 0, 0, zone)
	next := seen.Add(time.Minute)
	expiry := seen.Add(time.Hour)
	line := boardLivenessLineFrom(boardclient.HolderLiveness{Held: true, LeaseID: "lease-9",
		Holder: "brighton", LastSeenAt: seen, Beat: true, Silence: 1500 * time.Millisecond,
		Interval: 90 * time.Second, NextBeatBy: next, Overdue: true, ExpiresAt: expiry,
		Explain: "holder reported alive"})

	for name, pair := range map[string][2]*time.Time{
		"last seen":    {line.LastSeenAt, &seen},
		"next beat by": {line.NextBeatBy, &next},
		"expires at":   {line.ExpiresAt, &expiry},
	} {
		got, want := pair[0], pair[1]
		if got == nil {
			t.Fatalf("%s: absent; want it carried", name)
		}
		if !got.Equal(*want) {
			t.Fatalf("%s: got=%v; want the same instant as %v", name, got, want)
		}
		if got.Location() != time.UTC {
			t.Fatalf("%s: zone=%v; want UTC so two machines read one line the same way", name, got.Location())
		}
	}
	// Durations go out as whole seconds, truncated rather than rounded, so a
	// line never claims more silence than has actually passed.
	if line.SilenceSeconds != 1 || line.IntervalSeconds != 90 {
		t.Fatalf("silence=%d interval=%d; want 1 and 90", line.SilenceSeconds, line.IntervalSeconds)
	}
	if !line.Held || !line.Beat || !line.Overdue || line.LeaseID != "lease-9" ||
		line.Holder != "brighton" || line.Explain != "holder reported alive" {
		t.Fatalf("line=%+v; want the report carried through unchanged", line)
	}
}
