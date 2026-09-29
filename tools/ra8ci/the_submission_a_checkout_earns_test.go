// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"context"
	"encoding/json"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/runclient"
)

// A submission is the only run command that speaks about the tree the operator
// is standing in. Everything it sends is derived, not typed: the commit and the
// snapshot digest come from git, the catalog digest from the embedded copy, the
// task keys from the order the tasks were named. The refusals in front of that
// are already pinned; what is pinned here is the submission itself, driven over
// a real client against a stand-in plane, with a real checkout underneath.

// submittableCheckout plants a git repository holding the catalog files this
// binary was built from, commits it clean, and makes it the working directory.
// The catalog is copied rather than minted: VerifyCheckout compares the
// checkout's digest against the embedded one, so only the real files pass.
func submittableCheckout(t *testing.T) string {
	t.Helper()
	if _, err := exec.LookPath("git"); err != nil {
		t.Skip("git is unavailable")
	}
	root := t.TempDir()
	resolved, err := filepath.EvalSymlinks(root)
	if err != nil {
		t.Fatal(err)
	}
	base := filepath.Join(resolved, "tools", "ra8ci", "catalog")
	if err := os.MkdirAll(base, 0o755); err != nil {
		t.Fatal(err)
	}
	for _, name := range []string{"tasks.json", "sha256.txt"} {
		body, err := os.ReadFile(filepath.Join("catalog", name))
		if err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(filepath.Join(base, name), body, 0o644); err != nil {
			t.Fatal(err)
		}
	}
	for _, args := range [][]string{
		{"init", "-q", "-b", "ra8ci/dev"},
		{"add", "-A"},
		{"-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid",
			"commit", "-q", "-m", "catalog"},
	} {
		command := exec.Command("git", append([]string{"-C", resolved}, args...)...)
		if output, err := command.CombinedOutput(); err != nil {
			t.Fatalf("git %s: %v (%s)", strings.Join(args, " "), err, output)
		}
	}
	t.Chdir(resolved)
	return resolved
}

// askedPlane records what a submission actually put on the wire.
type submissionPlane struct {
	mutex   sync.Mutex
	method  string
	path    string
	key     string
	request runclient.SubmitRequest
}

// serve answers one receipt and keeps the request that earned it.
func (plane *submissionPlane) serve(t *testing.T, receipt runclient.Receipt) {
	t.Helper()
	servingRuns(t, func(writer http.ResponseWriter, request *http.Request) {
		plane.mutex.Lock()
		defer plane.mutex.Unlock()
		plane.method = request.Method
		plane.path = request.URL.Path
		plane.key = request.Header.Get("Idempotency-Key")
		if err := json.NewDecoder(request.Body).Decode(&plane.request); err != nil {
			t.Errorf("the submission was not readable: %v", err)
			writer.WriteHeader(http.StatusBadRequest)
			return
		}
		writer.Header().Set("Content-Type", "application/json")
		if err := json.NewEncoder(writer).Encode(receipt); err != nil {
			t.Errorf("the stand-in plane could not answer: %v", err)
		}
	})
}

func admitted() runclient.Receipt {
	return runclient.Receipt{ID: answeredRun, State: "queued"}
}

func TestSubmitSendsWhatTheCheckoutSaysAndSpeaksTheReceipt(t *testing.T) {
	root := submittableCheckout(t)
	plane := &submissionPlane{}
	plane.serve(t, admitted())

	spoke, err := spoken(t, func() error {
		return submitRun(context.Background(), []string{"--idempotency-key", "key-1", "ascii", "copyright"})
	})
	if err != nil {
		t.Fatalf("a submission from a clean checkout was refused: %v", err)
	}

	var receipt runclient.Receipt
	if err := json.Unmarshal([]byte(spoke), &receipt); err != nil {
		t.Fatalf("stdout was not a receipt: %v (%q)", err, spoke)
	}
	if receipt.ID != answeredRun || receipt.State != "queued" {
		t.Fatalf("receipt=%+v; want the one the plane answered", receipt)
	}

	if plane.method != http.MethodPost || plane.path != "/v1/runs" {
		t.Fatalf("submission went %s %s; want POST /v1/runs", plane.method, plane.path)
	}
	if plane.key != "key-1" {
		t.Fatalf("Idempotency-Key=%q; want the key the operator typed", plane.key)
	}
	sent := plane.request
	if sent.Trigger != "cli" {
		t.Fatalf("trigger=%q; want cli", sent.Trigger)
	}
	if sent.Source.Repository != "bsikar/ra8-firmware" {
		t.Fatalf("repo=%q; want the default repository", sent.Source.Repository)
	}
	if sent.Source.Branch != "ra8ci/dev" {
		t.Fatalf("branch=%q; want the branch the checkout is on", sent.Source.Branch)
	}
	// The commit and the digest are the checkout's, not the caller's: read
	// them back off the same tree rather than trusting the numbers sent.
	commit := gitSpoke(t, root, "rev-parse", "HEAD")
	if sent.Source.CommitSHA != commit {
		t.Fatalf("commit=%q; want the checkout's HEAD %q", sent.Source.CommitSHA, commit)
	}
	if len(sent.Source.SnapshotSHA256) != 64 {
		t.Fatalf("snapshot digest=%q; want a sha256 of the pinned tree", sent.Source.SnapshotSHA256)
	}
	embedded, err := catalog.Load()
	if err != nil {
		t.Fatal(err)
	}
	if sent.CatalogDigest != embedded.Digest() {
		t.Fatalf("catalog digest=%q; want the embedded %q", sent.CatalogDigest, embedded.Digest())
	}
	if len(sent.Tasks) != 2 {
		t.Fatalf("tasks=%+v; want the two named", sent.Tasks)
	}
	// Keys number the tasks in the order they were named, so a dependency
	// written against task-002 means the second task the operator typed.
	if sent.Tasks[0].Key != "task-001" || sent.Tasks[0].Name != "ascii" {
		t.Fatalf("first task=%+v; want task-001 ascii", sent.Tasks[0])
	}
	if sent.Tasks[1].Key != "task-002" || sent.Tasks[1].Name != "copyright" {
		t.Fatalf("second task=%+v; want task-002 copyright", sent.Tasks[1])
	}
	for _, task := range sent.Tasks {
		if task.Args == nil || task.DependsOnKeys == nil {
			t.Fatalf("task %q sent a null list: %+v", task.Name, task)
		}
	}
}

func TestSubmitCarriesTheRepositoryTheEnvironmentNames(t *testing.T) {
	submittableCheckout(t)
	plane := &submissionPlane{}
	plane.serve(t, admitted())
	t.Setenv("RA8CI_REPOSITORY", "bsikar/ra8-emulator")

	if _, err := spoken(t, func() error {
		return submitRun(context.Background(), []string{"--idempotency-key", "key-2", "ascii"})
	}); err != nil {
		t.Fatalf("a submission naming another repository was refused: %v", err)
	}
	if plane.request.Source.Repository != "bsikar/ra8-emulator" {
		t.Fatalf("repo=%q; want the one the environment named", plane.request.Source.Repository)
	}
}

func TestSubmitRefusesWhatThePlaneWillNotHaveToJudge(t *testing.T) {
	cases := map[string]struct {
		args []string
		want string
	}{
		"a task no catalog has": {
			[]string{"--idempotency-key", "key-3", "not-a-task"}, `unknown task "not-a-task"`},
		"the same task twice": {
			[]string{"--idempotency-key", "key-3", "ascii", "ascii"},
			`task "ascii" appears more than once`},
		"a value the task never declared": {
			[]string{"--idempotency-key", "key-3", "ascii", "depth=2"}, `task "ascii"`},
	}
	for name, testCase := range cases {
		t.Run(name, func(t *testing.T) {
			submittableCheckout(t)
			// No plane at all: each of these has to be refused locally,
			// before a submission is ever spent on it.
			t.Setenv("RA8CI_SERVER_URL", "")
			spoke, err := spoken(t, func() error {
				return submitRun(context.Background(), testCase.args)
			})
			if err == nil {
				t.Fatal("the submission was accepted")
			}
			if !strings.Contains(err.Error(), testCase.want) {
				t.Fatalf("err=%v; want it to carry %q", err, testCase.want)
			}
			if strings.TrimSpace(spoke) != "" {
				t.Fatalf("stdout=%q; want nothing said for a refused submission", spoke)
			}
		})
	}
}

func TestSubmitRefusesADirtyCheckout(t *testing.T) {
	root := submittableCheckout(t)
	if err := os.WriteFile(filepath.Join(root, "scratch.txt"), []byte("uncommitted\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	t.Setenv("RA8CI_SERVER_URL", "")

	_, err := spoken(t, func() error {
		return submitRun(context.Background(), []string{"--idempotency-key", "key-4", "ascii"})
	})
	if err == nil {
		t.Fatal("a submission was accepted from a tree with uncommitted work in it")
	}
	// The snapshot is what a run is replayed from, so the refusal has to say
	// the tree is the problem rather than anything about the plane.
	if !strings.Contains(err.Error(), "clean pinned source snapshot") {
		t.Fatalf("err=%v; want it to name the snapshot", err)
	}
}

func TestSubmitRefusesAReceiptThePlaneCouldNotHaveMeant(t *testing.T) {
	submittableCheckout(t)
	plane := &submissionPlane{}
	plane.serve(t, runclient.Receipt{ID: answeredRun, State: "sprinting"})

	spoke, err := spoken(t, func() error {
		return submitRun(context.Background(), []string{"--idempotency-key", "key-5", "ascii"})
	})
	if err == nil {
		t.Fatal("a receipt in a state the plane has no machine for was believed")
	}
	if !strings.Contains(err.Error(), "sprinting") {
		t.Fatalf("err=%v; want it to name the state it would not take", err)
	}
	if strings.TrimSpace(spoke) != "" {
		t.Fatalf("stdout=%q; want no receipt written for one that was refused", spoke)
	}
}

// gitSpoke reads one value back out of the fixture checkout.
func gitSpoke(t *testing.T, root string, args ...string) string {
	t.Helper()
	output, err := exec.Command("git", append([]string{"-C", root}, args...)...).Output()
	if err != nil {
		t.Fatalf("git %s: %v", strings.Join(args, " "), err)
	}
	return strings.TrimSpace(string(output))
}
