package server

import (
	"context"
	"crypto/tls"
	"crypto/x509"
	"encoding/json"
	"errors"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

func TestNewRequiresAllDependencies(t *testing.T) {
	if _, err := New(nil, nil); err == nil {
		t.Fatal("server accepted missing store and catalog")
	}
	cat, err := catalog.Load()
	if err != nil {
		t.Fatal(err)
	}
	if _, err := New(nil, cat); err == nil {
		t.Fatal("server accepted missing store")
	}
}

func TestAgentClaimRequiresReviewedCommit(t *testing.T) {
	cat, err := catalog.Load()
	if err != nil {
		t.Fatal(err)
	}
	api, err := New(&store.Store{}, cat)
	if err != nil {
		t.Fatal(err)
	}
	request := httptest.NewRequest(http.MethodPost, "/v1/agents/me/claim", nil)
	response := httptest.NewRecorder()
	api.Handler().ServeHTTP(response, request)
	if response.Code != http.StatusServiceUnavailable {
		t.Fatalf("untrusted remote dispatch status=%d", response.Code)
	}
	if _, err := NewWithOptions(&store.Store{}, cat, nil, "not-a-commit"); err == nil {
		t.Fatal("invalid trusted commit accepted")
	}
}

func TestAgentCertificateExpiryIsRecheckedPerRequest(t *testing.T) {
	leaf := &x509.Certificate{Raw: []byte("expired"),
		NotBefore: time.Now().Add(-2 * time.Hour), NotAfter: time.Now().Add(-time.Hour)}
	request := httptest.NewRequest(http.MethodPost, "/v1/agents/me/claim", nil)
	request.TLS = &tls.ConnectionState{PeerCertificates: []*x509.Certificate{leaf},
		VerifiedChains: [][]*x509.Certificate{{leaf}}}
	if _, err := verifiedAgentCertificate(request); err == nil {
		t.Fatal("expired certificate reused on long-lived connection")
	}
}

func TestMTLSAuthorizerRejectsNoVerifiedPeer(t *testing.T) {
	request := httptest.NewRequest("GET", "/v1/runs", nil)
	if _, err := (MTLSAuthorizer{Store: &store.Store{}}).Authorize(request, "bsikar/ra8-firmware", "read"); err == nil {
		t.Fatal("unauthenticated HTTP request was authorized")
	}
}

func TestMTLSAuthorizerRejectsEmptyVerifiedChain(t *testing.T) {
	request := httptest.NewRequest("GET", "/v1/runs", nil)
	request.TLS = &tls.ConnectionState{VerifiedChains: [][]*x509.Certificate{{}}}
	if _, err := (MTLSAuthorizer{Store: &store.Store{}}).Authorize(request, "bsikar/ra8-firmware", "read"); err == nil {
		t.Fatal("empty verified chain was authorized")
	}
}

func TestReadinessRunsEveryConfiguredDependencyAndFailsClosed(t *testing.T) {
	failure := errors.New("backup attestation expired")
	calls := 0
	api := &Server{readinessChecks: []func(context.Context) error{
		func(context.Context) error { calls++; return nil },
		func(context.Context) error { calls++; return failure },
		func(context.Context) error { calls++; return nil },
	}}
	if err := api.checkReadiness(context.Background()); !errors.Is(err, failure) {
		t.Fatalf("readiness error = %v, want backup failure", err)
	}
	if calls != 2 {
		t.Fatalf("ran %d readiness checks, want 2", calls)
	}
}

func TestPersistedTaskArgumentsCarryOnlyCatalogHILMetadata(t *testing.T) {
	definition := catalog.Task{HIL: &catalog.HILTask{
		BoardID: "ek-ra8d2", BoardModel: "EK-RA8D2",
		ManifestPath:  "examples/ek_ra8d2/hw_validated/hil/demo/hil.conf",
		ProgramFamily: "uart-demo", Mode: "uart_scrape",
		ObservationStep: "observe", FlashRestoreSeconds: 10,
	}}
	raw, err := persistedTaskArguments(definition, nil)
	if err != nil {
		t.Fatal(err)
	}
	var stored struct {
		Arguments []string         `json:"argv"`
		HIL       *catalog.HILTask `json:"hil"`
	}
	if err := json.Unmarshal(raw, &stored); err != nil {
		t.Fatal(err)
	}
	if len(stored.Arguments) != 0 || stored.HIL == nil || *stored.HIL != *definition.HIL {
		t.Fatalf("persisted HIL task identity changed: %+v", stored)
	}
}

// TestPersistedTaskArgumentsBindsArgvAtThePlane is the admission property:
// the plane writes argv it derived itself from the submitter's names and the
// reviewed schema, so a caller cannot state an argv element at all.
func TestPersistedTaskArgumentsBindsArgvAtThePlane(t *testing.T) {
	definition := catalog.Task{Name: "hil-run", ArgsSchema: catalog.ArgsSchema{
		Positional: []string{"target"}, Flags: []string{"profile"}}}
	values := map[string]string{"target": "ra8p1", "profile": "release"}
	raw, err := persistedTaskArguments(definition, values)
	if err != nil {
		t.Fatal(err)
	}
	var stored struct {
		Arguments []string          `json:"argv"`
		Values    map[string]string `json:"values"`
	}
	if err := json.Unmarshal(raw, &stored); err != nil {
		t.Fatal(err)
	}
	want := []string{"ra8p1", "--profile=release"}
	if strings.Join(stored.Arguments, "\x00") != strings.Join(want, "\x00") {
		t.Fatalf("persisted argv = %q, want %q", stored.Arguments, want)
	}
	if len(stored.Values) != 2 || stored.Values["target"] != "ra8p1" || stored.Values["profile"] != "release" {
		t.Fatalf("persisted values = %v", stored.Values)
	}
	// And the row re-validates against the catalog that wrote it.
	if err := definition.ValidatePersistedArguments(stored.Values, stored.Arguments); err != nil {
		t.Fatalf("a row the plane just wrote does not re-validate: %v", err)
	}
	for name, values := range map[string]map[string]string{
		"undeclared": {"target": "ra8p1", "quiet": "1"},
		"missing":    {"profile": "release"},
		"shell":      {"target": "a;rm -rf /"},
	} {
		if _, err := persistedTaskArguments(definition, values); err == nil {
			t.Fatalf("%s values were admitted", name)
		}
	}
}

// TestPersistedTaskArgumentsKeepsTheStoredShapeForAnArgumentFreeTask pins that
// every task in the v1 catalog still writes exactly {"argv":[]}: no values
// key appears, so nothing that reads a row written before this changes.
func TestPersistedTaskArgumentsKeepsTheStoredShapeForAnArgumentFreeTask(t *testing.T) {
	raw, err := persistedTaskArguments(catalog.Task{Name: "test-go"}, nil)
	if err != nil {
		t.Fatal(err)
	}
	if string(raw) != `{"argv":[]}` {
		t.Fatalf("stored shape = %s, want {\"argv\":[]}", raw)
	}
	if _, err := persistedTaskArguments(catalog.Task{Name: "test-go"},
		map[string]string{"profile": "release"}); err == nil {
		t.Fatal("values were admitted for a task that declares none")
	}
}
