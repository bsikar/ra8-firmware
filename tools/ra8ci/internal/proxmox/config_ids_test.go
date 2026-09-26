// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package proxmox

import (
	"encoding/pem"
	"errors"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func idSet(ids ...int) map[int]struct{} {
	set := make(map[int]struct{}, len(ids))
	for _, id := range ids {
		set[id] = struct{}{}
	}
	return set
}

func TestDisjointVMIDListsAreAccepted(t *testing.T) {
	if err := checkDisjointIDs(idSet(9000, 9002), idSet(9001, 9003)); err != nil {
		t.Fatalf("disjoint lists: %v", err)
	}
}

func TestIDInBothListsIsRefused(t *testing.T) {
	err := checkDisjointIDs(idSet(9000, 9001), idSet(9001))
	if !errors.Is(err, ErrInvalid) {
		t.Fatalf("want ErrInvalid, got %v", err)
	}
	if !strings.Contains(err.Error(), "9001") {
		t.Fatalf("refusal must name the contradictory ID: %v", err)
	}
	if strings.Contains(err.Error(), "9000") {
		t.Fatalf("refusal must not name an ID that is only allowlisted: %v", err)
	}
}

func TestEveryOverlappingIDIsNamedInOrder(t *testing.T) {
	err := checkDisjointIDs(idSet(9005, 9001, 9003), idSet(9003, 9001, 9005))
	if !errors.Is(err, ErrInvalid) {
		t.Fatalf("want ErrInvalid, got %v", err)
	}
	if !strings.Contains(err.Error(), "9001, 9003, 9005") {
		t.Fatalf("overlapping IDs must be reported sorted, got %v", err)
	}
}

func TestEmptyListsAreDisjoint(t *testing.T) {
	if err := checkDisjointIDs(nil, nil); err != nil {
		t.Fatalf("empty lists: %v", err)
	}
	if err := checkDisjointIDs(idSet(9000), nil); err != nil {
		t.Fatalf("empty template list: %v", err)
	}
}

// configFixture builds the operator-controlled half of a Config against a
// throwaway API server, so a test can vary only the two ID lists.
func configFixture(t *testing.T) Config {
	t.Helper()
	server := httptest.NewTLSServer(http.HandlerFunc(newFake().serve))
	t.Cleanup(server.Close)
	cert := server.Certificate()
	if cert == nil {
		t.Fatal("test server has no certificate")
	}
	dir := t.TempDir()
	ca := filepath.Join(dir, "ca.pem")
	if err := os.WriteFile(ca, pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: cert.Raw}), 0600); err != nil {
		t.Fatal(err)
	}
	token := filepath.Join(dir, "token")
	if err := os.WriteFile(token, []byte("ra8ci@pve!client=secret-token\n"), 0600); err != nil {
		t.Fatal(err)
	}
	return Config{
		Endpoint: server.URL, CAFile: ca, TokenFile: token,
		Node: "pve", Pool: "ra8-tf-lab", Storage: "ra8-tf-lab",
		Bridges: []string{"vmbr8", "vmbr9"}, RequestTimeout: time.Second,
		OperationTimeout: time.Second, TaskPollInterval: time.Millisecond,
	}
}

func TestNewRefusesATemplateIDThatIsAlsoDisposable(t *testing.T) {
	cfg := configFixture(t)
	cfg.AllowedVMIDs = []int{9000, 9001}
	cfg.TemplateVMIDs = []int{9001}
	_, err := New(cfg)
	if !errors.Is(err, ErrInvalid) {
		t.Fatalf("want ErrInvalid, got %v", err)
	}
	if !strings.Contains(err.Error(), "reviewed template and a disposable guest") {
		t.Fatalf("refusal must say what the contradiction is: %v", err)
	}
	if !strings.Contains(err.Error(), "9001") {
		t.Fatalf("refusal must name the contradictory ID: %v", err)
	}
}

func TestNewStillAcceptsSeparateTemplateAndGuestIDs(t *testing.T) {
	cfg := configFixture(t)
	cfg.AllowedVMIDs = []int{9000}
	cfg.TemplateVMIDs = []int{9001}
	if _, err := New(cfg); err != nil {
		t.Fatalf("separate lists must still build a client: %v", err)
	}
}
