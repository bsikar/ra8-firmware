package spool

import (
	"errors"
	"os"
	"path/filepath"
	"runtime"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/executor"
)

func TestLifecycleKeepsUnsyncedRecordUntilReceipt(t *testing.T) {
	spool, err := Open(filepath.Join(t.TempDir(), "outbox"))
	if err != nil {
		t.Fatal(err)
	}
	started, err := spool.Begin("format-check", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")
	if err != nil {
		t.Fatal(err)
	}
	if len(started.ID) != 32 || started.SyncState != "running" {
		t.Fatalf("bad start: %+v", started)
	}
	result := executor.Result{TaskName: "format-check", StartedAt: time.Now().UTC(), ExitCode: 7}
	finished, err := spool.Finish(started, result, errors.New("step failed"))
	if err != nil {
		t.Fatal(err)
	}
	if finished.SyncState != "unsynced" || finished.Result.ExitCode != 7 {
		t.Fatalf("bad finish: %+v", finished)
	}
	pending, err := spool.Pending()
	if err != nil {
		t.Fatal(err)
	}
	if len(pending) != 1 || pending[0].ID != started.ID {
		t.Fatalf("pending: %+v", pending)
	}
	if err := spool.MarkSynced(started.ID, "server-run-1"); err != nil {
		t.Fatal(err)
	}
	pending, err = spool.Pending()
	if err != nil {
		t.Fatal(err)
	}
	if len(pending) != 0 {
		t.Fatalf("synced entry pending: %+v", pending)
	}
	if err := spool.MarkSynced(started.ID, "different-run"); err == nil {
		t.Fatal("append-only receipt overwritten")
	}
	if _, err := os.Stat(filepath.Join(spool.directory, started.ID+".started.json")); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(filepath.Join(spool.directory, started.ID+".finished.json")); err != nil {
		t.Fatal(err)
	}
}

func TestBeginWithMetadataFreezesSourceAndTaskBeforeExecution(t *testing.T) {
	s, err := Open(filepath.Join(t.TempDir(), "outbox"))
	if err != nil {
		t.Fatal(err)
	}
	metadata := Metadata{Source: SourceIdentity{Repository: "bsikar/ra8-firmware",
		Branch: "feature", CommitSHA: "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
		SnapshotSHA256: "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
		Verification:   "verified"}, Tier: "required", Scope: "safe-local-read-only",
		DeadlineSeconds: 900, Args: []string{}}
	entry, err := s.BeginWithMetadata("format-check",
		"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc", metadata)
	if err != nil || entry.SchemaVersion != 2 || entry.Source.SnapshotSHA256 != metadata.Source.SnapshotSHA256 {
		t.Fatalf("metadata was not frozen: %+v %v", entry, err)
	}
	metadata.Source.SnapshotSHA256 = "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd"
	if entry.Source.SnapshotSHA256 == metadata.Source.SnapshotSHA256 {
		t.Fatal("caller mutated persisted source identity")
	}
	if _, err := s.BeginWithMetadata("format-check",
		"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc",
		Metadata{Source: SourceIdentity{Repository: "repo", CommitSHA: "bad", Verification: "verified"},
			Tier: "required", Scope: "safe-local-read-only", DeadlineSeconds: 900}); err == nil {
		t.Fatal("invalid verified source accepted")
	}
	metadata.Source.Verification = "unverified"
	metadata.Source.SnapshotSHA256 = ""
	if _, err := s.BeginWithMetadata("format-check",
		"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc", metadata); err != nil {
		t.Fatalf("dirty local source should remain executable and explicitly unverified: %v", err)
	}
}

func TestOpenRejectsSharedDirectoryAndSymlink(t *testing.T) {
	if _, err := Open("relative"); err == nil {
		t.Fatal("relative state path accepted")
	}
	base := t.TempDir()
	target := filepath.Join(base, "target")
	if err := os.Mkdir(target, 0700); err != nil {
		t.Fatal(err)
	}
	link := filepath.Join(base, "link")
	if err := os.Symlink(target, link); err == nil {
		if _, err := Open(link); err == nil {
			t.Fatal("symlink spool accepted")
		}
	}
	if runtime.GOOS != "windows" {
		if err := os.Chmod(target, 0777); err != nil {
			t.Fatal(err)
		}
		if _, err := Open(target); err == nil {
			t.Fatal("shared directory accepted")
		}
	}
}

func TestDefaultDirectoryModes(t *testing.T) {
	base := t.TempDir()
	env := map[string]string{"RA8CI_STATE_DIR": filepath.Join(base, "custom")}
	get := func(key string) string { return env[key] }
	home := func() (string, error) { return base, nil }
	path, err := defaultDirectory("linux", get, home)
	if err != nil || path != env["RA8CI_STATE_DIR"] {
		t.Fatalf("custom path=%q err=%v", path, err)
	}
	env["RA8CI_STATE_DIR"] = "relative"
	if _, err := defaultDirectory("linux", get, home); err == nil {
		t.Fatal("relative override accepted")
	}
	delete(env, "RA8CI_STATE_DIR")
	env["LOCALAPPDATA"] = base
	path, err = defaultDirectory("windows", get, home)
	if err != nil || path != filepath.Join(base, "ra8ci", "outbox") {
		t.Fatalf("Windows path=%q err=%v", path, err)
	}
	delete(env, "LOCALAPPDATA")
	if _, err := defaultDirectory("windows", get, home); err == nil {
		t.Fatal("missing Windows base accepted")
	}
	env["XDG_STATE_HOME"] = base
	path, err = defaultDirectory("linux", get, home)
	if err != nil || path != filepath.Join(base, "ra8ci", "outbox") {
		t.Fatalf("XDG path=%q err=%v", path, err)
	}
	env["XDG_STATE_HOME"] = "relative"
	if _, err := defaultDirectory("linux", get, home); err == nil {
		t.Fatal("relative XDG base accepted")
	}
	delete(env, "XDG_STATE_HOME")
	path, err = defaultDirectory("linux", get, home)
	if err != nil || path != filepath.Join(base, ".local", "state", "ra8ci", "outbox") {
		t.Fatalf("home path=%q err=%v", path, err)
	}
	if _, err := defaultDirectory("linux", get, func() (string, error) { return "", errors.New("home unavailable") }); err == nil {
		t.Fatal("home failure ignored")
	}
}

func TestSpoolRejectsInvalidTransitionsAndCorruptRecords(t *testing.T) {
	s, err := Open(filepath.Join(t.TempDir(), "outbox"))
	if err != nil {
		t.Fatal(err)
	}
	if _, err := s.Begin("", "short"); err == nil {
		t.Fatal("empty task accepted")
	}
	if _, err := s.Begin("format", "short"); err == nil {
		t.Fatal("short digest accepted")
	}
	if _, err := s.Finish(Entry{ID: "wrong", SyncState: "running"}, executor.Result{}, nil); err == nil {
		t.Fatal("invalid ID accepted")
	}
	if _, err := s.Finish(Entry{ID: "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", SyncState: "running"}, executor.Result{}, nil); err == nil {
		t.Fatal("missing start accepted")
	}
	if err := s.MarkSynced("wrong", "server"); err == nil {
		t.Fatal("invalid sync ID accepted")
	}
	if err := s.MarkSynced("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "server"); err == nil {
		t.Fatal("missing finish accepted")
	}
	if _, err := s.Pending(); err != nil {
		t.Fatal(err)
	}
	if err := s.write("../escape", map[string]string{"x": "y"}); err == nil {
		t.Fatal("path traversal accepted")
	}
	if err := os.WriteFile(filepath.Join(s.directory, "bad.finished.json"), []byte("{}"), 0600); err != nil {
		t.Fatal(err)
	}
	if _, err := s.Pending(); err == nil {
		t.Fatal("invalid file name accepted")
	}
	if err := os.Remove(filepath.Join(s.directory, "bad.finished.json")); err != nil {
		t.Fatal(err)
	}
	valid := "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
	file := filepath.Join(s.directory, valid+".finished.json")
	if err := os.WriteFile(file, []byte("not-json"), 0600); err != nil {
		t.Fatal(err)
	}
	if _, err := s.Pending(); err == nil {
		t.Fatal("invalid JSON accepted")
	}
	if err := os.WriteFile(file, []byte(`{"id":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","sync_state":"running"}`), 0600); err != nil {
		t.Fatal(err)
	}
	if _, err := s.Pending(); err == nil {
		t.Fatal("unfinished record accepted")
	}
	if _, err := (*Spool)(nil).Pending(); err == nil {
		t.Fatal("nil spool accepted")
	}
	if err := s.write("collision.json", map[string]string{"first": "yes"}); err != nil {
		t.Fatal(err)
	}
	if err := s.write("collision.json", map[string]string{"second": "no"}); err == nil {
		t.Fatal("existing append-only entry overwritten")
	}
	if err := s.write("invalid.json", func() {}); err == nil {
		t.Fatal("unserializable entry accepted")
	}
	badDirectory := filepath.Join(t.TempDir(), "file")
	if err := os.WriteFile(badDirectory, []byte("x"), 0600); err != nil {
		t.Fatal(err)
	}
	if err := (&Spool{directory: badDirectory}).write("x.json", 1); err == nil {
		t.Fatal("invalid spool directory accepted")
	}
}

func TestDefaultDirectoryUsesOverride(t *testing.T) {
	want := filepath.Join(t.TempDir(), "state")
	t.Setenv("RA8CI_STATE_DIR", want)
	got, err := DefaultDirectory()
	if err != nil || got != want {
		t.Fatalf("path=%q err=%v", got, err)
	}
}
