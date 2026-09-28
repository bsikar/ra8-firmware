// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package runclient

import (
	"context"
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/json"
	"encoding/pem"
	"fmt"
	"math/big"
	"net"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// clientMaterial is one CA, one server identity signed by it, and one client
// identity signed by it, written where New can read them.
type clientMaterial struct {
	pool       *x509.CertPool
	server     tls.Certificate
	caPath     string
	certPath   string
	keyPath    string
	otherCA    string
	expiredCrt string
	expiredKey string
}

func mintClientMaterial(t *testing.T) clientMaterial {
	t.Helper()
	dir := t.TempDir()
	now := time.Now()

	authority := func(name string) (*x509.Certificate, *ecdsa.PrivateKey, string) {
		t.Helper()
		key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
		if err != nil {
			t.Fatal(err)
		}
		template := &x509.Certificate{SerialNumber: big.NewInt(1), Subject: pkix.Name{CommonName: name},
			NotBefore: now.Add(-time.Hour), NotAfter: now.Add(time.Hour), IsCA: true,
			BasicConstraintsValid: true, KeyUsage: x509.KeyUsageCertSign | x509.KeyUsageCRLSign}
		der, err := x509.CreateCertificate(rand.Reader, template, template, &key.PublicKey, key)
		if err != nil {
			t.Fatal(err)
		}
		parsed, err := x509.ParseCertificate(der)
		if err != nil {
			t.Fatal(err)
		}
		path := filepath.Join(dir, name+"-ca.pem")
		if err := os.WriteFile(path, pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: der}), 0o600); err != nil {
			t.Fatal(err)
		}
		return parsed, key, path
	}

	ca, caKey, caPath := authority("trusted")
	_, _, otherCAPath := authority("stranger")

	leaf := func(serial int64, name string, server bool, notAfter time.Time) (tls.Certificate, string, string) {
		t.Helper()
		key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
		if err != nil {
			t.Fatal(err)
		}
		template := &x509.Certificate{SerialNumber: big.NewInt(serial), NotBefore: now.Add(-time.Hour),
			NotAfter: notAfter, KeyUsage: x509.KeyUsageDigitalSignature}
		if server {
			template.Subject = pkix.Name{CommonName: "localhost"}
			template.DNSNames = []string{"localhost"}
			template.IPAddresses = []net.IP{net.ParseIP("127.0.0.1")}
			template.ExtKeyUsage = []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth}
		} else {
			template.Subject = pkix.Name{CommonName: "run client"}
			template.ExtKeyUsage = []x509.ExtKeyUsage{x509.ExtKeyUsageClientAuth}
		}
		der, err := x509.CreateCertificate(rand.Reader, template, ca, &key.PublicKey, caKey)
		if err != nil {
			t.Fatal(err)
		}
		encoded, err := x509.MarshalECPrivateKey(key)
		if err != nil {
			t.Fatal(err)
		}
		certPath := filepath.Join(dir, fmt.Sprintf("%s-%d-cert.pem", name, serial))
		keyPath := filepath.Join(dir, fmt.Sprintf("%s-%d-key.pem", name, serial))
		if err := os.WriteFile(certPath,
			pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: der}), 0o600); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(keyPath,
			pem.EncodeToMemory(&pem.Block{Type: "EC PRIVATE KEY", Bytes: encoded}), 0o600); err != nil {
			t.Fatal(err)
		}
		pair, err := tls.LoadX509KeyPair(certPath, keyPath)
		if err != nil {
			t.Fatal(err)
		}
		return pair, certPath, keyPath
	}

	serverPair, _, _ := leaf(2, "server", true, now.Add(time.Hour))
	_, certPath, keyPath := leaf(3, "client", false, now.Add(time.Hour))
	_, expiredCert, expiredKey := leaf(4, "expired", false, now.Add(-time.Minute))

	pool := x509.NewCertPool()
	pool.AddCert(ca)
	return clientMaterial{pool: pool, server: serverPair, caPath: caPath, certPath: certPath,
		keyPath: keyPath, otherCA: otherCAPath, expiredCrt: expiredCert, expiredKey: expiredKey}
}

// TestNewRefusesAnOriginItCannotHold pins the shapes of origin the run client
// refuses before it ever reads a file. An origin with a path, a query or an
// embedded user is refused rather than trimmed: a caller that believes it
// pinned a prefix and silently lost it would send every request somewhere it
// never named.
func TestNewRefusesAnOriginItCannotHold(t *testing.T) {
	material := mintClientMaterial(t)
	identity := func(url string) Config {
		return Config{ServerURL: url, CAFile: material.caPath,
			CertFile: material.certPath, KeyFile: material.keyPath}
	}
	for name, config := range map[string]Config{
		"nothing at all":       {},
		"plain http":           identity("http://localhost:8443"),
		"no scheme":            identity("localhost:8443"),
		"no host":              identity("https://"),
		"embedded user":        identity("https://user:secret@localhost"),
		"path prefix":          identity("https://localhost/v1"),
		"query":                identity("https://localhost?tenant=a"),
		"fragment":             identity("https://localhost#top"),
		"unparseable":          identity("https://loc alhost\x7f"),
		"no CA file named":     {ServerURL: "https://localhost", CertFile: material.certPath, KeyFile: material.keyPath},
		"no certificate named": {ServerURL: "https://localhost", CAFile: material.caPath, KeyFile: material.keyPath},
		"no key named":         {ServerURL: "https://localhost", CAFile: material.caPath, CertFile: material.certPath},
	} {
		t.Run(name, func(t *testing.T) {
			client, err := New(config)
			if err == nil || client != nil {
				t.Fatalf("origin accepted: client=%v err=%v", client, err)
			}
			if !strings.Contains(err.Error(), "HTTPS origin and mTLS identity") {
				t.Fatalf("refusal does not name the requirement: %v", err)
			}
		})
	}
}

// TestNewRefusesMaterialItCannotVerify pins the refusals that need the files
// themselves read: each one names which piece of material was at fault, which
// is what tells an operator whether to look at the trust roots or the identity.
func TestNewRefusesMaterialItCannotVerify(t *testing.T) {
	material := mintClientMaterial(t)
	garbage := filepath.Join(t.TempDir(), "garbage.pem")
	if err := os.WriteFile(garbage, []byte("not a certificate"), 0o600); err != nil {
		t.Fatal(err)
	}
	for name, testCase := range map[string]struct {
		config Config
		reason string
	}{
		"CA file absent": {Config{ServerURL: "https://localhost", CAFile: filepath.Join(t.TempDir(), "gone.pem"),
			CertFile: material.certPath, KeyFile: material.keyPath}, "read server CA"},
		"CA file is not PEM": {Config{ServerURL: "https://localhost", CAFile: garbage,
			CertFile: material.certPath, KeyFile: material.keyPath}, "run client server trust"},
		"identity absent": {Config{ServerURL: "https://localhost", CAFile: material.caPath,
			CertFile: filepath.Join(t.TempDir(), "gone.pem"), KeyFile: material.keyPath}, "run client identity"},
		"key does not match certificate": {Config{ServerURL: "https://localhost", CAFile: material.caPath,
			CertFile: material.certPath, KeyFile: material.expiredKey}, "run client identity"},
		"identity expired": {Config{ServerURL: "https://localhost", CAFile: material.caPath,
			CertFile: material.expiredCrt, KeyFile: material.expiredKey}, "run client identity"},
	} {
		t.Run(name, func(t *testing.T) {
			client, err := New(testCase.config)
			if err == nil || client != nil {
				t.Fatalf("material accepted: client=%v err=%v", client, err)
			}
			if !strings.Contains(err.Error(), testCase.reason) {
				t.Fatalf("refusal %q does not name %q", err, testCase.reason)
			}
		})
	}
}

// TestNewSpeaksMutualTLSAndRefusesARedirect is the built client's one live
// exchange: TLS 1.3 both ways against a server that demands a verified client
// certificate, and a redirect handed back as the status it is rather than
// followed to wherever the answer points.
func TestNewSpeaksMutualTLSAndRefusesARedirect(t *testing.T) {
	material := mintClientMaterial(t)
	run := store.Run{ID: "00000000-0000-7000-8000-00000000000a", State: "queued"}
	server := httptest.NewUnstartedServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.TLS == nil || len(r.TLS.VerifiedChains) == 0 || len(r.TLS.PeerCertificates) == 0 {
			t.Error("server did not authenticate the client certificate")
			w.WriteHeader(http.StatusUnauthorized)
			return
		}
		if r.TLS.Version != tls.VersionTLS13 {
			t.Errorf("negotiated TLS version = %x", r.TLS.Version)
		}
		if r.URL.Path == "/v1/runs/"+run.ID {
			w.Header().Set("Content-Type", "application/json")
			_ = json.NewEncoder(w).Encode(run)
			return
		}
		w.Header().Set("Location", "https://elsewhere.invalid/v1/runs")
		w.WriteHeader(http.StatusFound)
	}))
	server.TLS = &tls.Config{Certificates: []tls.Certificate{material.server},
		ClientAuth: tls.RequireAndVerifyClientCert, ClientCAs: material.pool, MinVersion: tls.VersionTLS13}
	server.StartTLS()
	defer server.Close()

	client, err := New(Config{ServerURL: server.URL, CAFile: material.caPath,
		CertFile: material.certPath, KeyFile: material.keyPath})
	if err != nil {
		t.Fatal(err)
	}
	defer client.Close()

	answered, err := client.Get(context.Background(), run.ID)
	if err != nil {
		t.Fatalf("mutual TLS read failed: %v", err)
	}
	if answered.ID != run.ID || answered.State != "queued" {
		t.Fatalf("unexpected run: %+v", answered)
	}
	if _, err := client.Cancel(context.Background(), "00000000-0000-7000-8000-00000000000b"); err == nil ||
		!strings.Contains(err.Error(), "HTTP 302") {
		t.Fatalf("redirect was followed rather than reported: %v", err)
	}
}

// TestNewKeepsTheOriginAndClosesQuietly pins the two small things around the
// transport: the origin is held with no path, and Close is safe on a nil
// client and on one built without a transport, so a deferred Close after a
// failed New is never itself the failure.
func TestNewKeepsTheOriginAndClosesQuietly(t *testing.T) {
	material := mintClientMaterial(t)
	for _, origin := range []string{"https://runs.invalid", "https://runs.invalid/", "https://runs.invalid:8443"} {
		client, err := New(Config{ServerURL: origin, CAFile: material.caPath,
			CertFile: material.certPath, KeyFile: material.keyPath})
		if err != nil {
			t.Fatalf("%s: %v", origin, err)
		}
		if client.base.Path != "" || client.base.RawPath != "" || client.base.Scheme != "https" {
			t.Fatalf("%s: origin kept a path: %+v", origin, client.base)
		}
		if client.http == nil || client.http.Timeout != 30*time.Second {
			t.Fatalf("%s: transport is not bounded: %+v", origin, client.http)
		}
		client.Close()
		client.Close()
	}
	var absent *Client
	absent.Close()
	(&Client{}).Close()
}

// eventPage is the page an honest server would answer with for one run.
func eventPage(runID string, from int64, count int, hasMore bool) store.RunEventPage {
	page := store.RunEventPage{RunID: runID, NextAfter: from + int64(count), HasMore: hasMore}
	for index := 0; index < count; index++ {
		page.Events = append(page.Events, store.RunEvent{
			Sequence:   from + int64(index) + 1,
			ID:         fmt.Sprintf("00000000-0000-7000-8000-0000000000%02d", index+1),
			Kind:       "state_changed",
			Data:       json.RawMessage(`{"state":"running"}`),
			HappenedAt: time.Now().UTC(),
		})
	}
	return page
}

// servingEvents answers every event request with whatever the supplied
// function returns, and records the query the client actually asked.
func servingEvents(t *testing.T, answer func(after, limit string) store.RunEventPage) (*Client, *httptest.Server) {
	t.Helper()
	server := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodGet || !strings.HasSuffix(r.URL.Path, "/events") {
			t.Errorf("request = %s %s", r.Method, r.URL.Path)
		}
		w.Header().Set("Content-Type", "application/json")
		_ = json.NewEncoder(w).Encode(answer(r.URL.Query().Get("after"), r.URL.Query().Get("limit")))
	}))
	t.Cleanup(server.Close)
	return testClient(server), server
}

// TestEventsRefusesAPageRequestItCannotMake pins what the client settles
// before a request leaves: a page nobody could answer is refused here rather
// than spent on the server.
func TestEventsRefusesAPageRequestItCannotMake(t *testing.T) {
	client, _ := servingEvents(t, func(string, string) store.RunEventPage {
		t.Error("an invalid page request reached the server")
		return store.RunEventPage{}
	})
	valid := "00000000-0000-7000-8000-00000000000a"
	for name, request := range map[string]struct {
		runID string
		after int64
		limit int
	}{
		"no run":           {"", 0, 10},
		"malformed run":    {"not-a-run", 0, 10},
		"negative cursor":  {valid, -1, 10},
		"no limit":         {valid, 0, 0},
		"negative limit":   {valid, 0, -1},
		"limit over bound": {valid, 0, store.MaxEventPageSize + 1},
		"limit far over":   {valid, 0, 1 << 20},
	} {
		t.Run(name, func(t *testing.T) {
			page, err := client.Events(context.Background(), request.runID, request.after, request.limit)
			if err == nil || !strings.Contains(err.Error(), "invalid run event page request") {
				t.Fatalf("page request accepted: %v", err)
			}
			if len(page.Events) != 0 || page.RunID != "" {
				t.Fatalf("a refused request still answered a page: %+v", page)
			}
		})
	}
}

// TestEventsAsksForTheWindowItWasGiven pins the query the client puts on the
// wire and the page it hands back when the server answers honestly.
func TestEventsAsksForTheWindowItWasGiven(t *testing.T) {
	runID := "00000000-0000-7000-8000-00000000000a"
	var askedAfter, askedLimit string
	client, _ := servingEvents(t, func(after, limit string) store.RunEventPage {
		askedAfter, askedLimit = after, limit
		return eventPage(runID, 7, 3, false)
	})
	page, err := client.Events(context.Background(), runID, 7, 3)
	if err != nil {
		t.Fatal(err)
	}
	if askedAfter != "7" || askedLimit != "3" {
		t.Fatalf("query sent after=%q limit=%q", askedAfter, askedLimit)
	}
	if page.RunID != runID || len(page.Events) != 3 || page.NextAfter != 10 || page.HasMore {
		t.Fatalf("unexpected page: %+v", page)
	}
	for index, event := range page.Events {
		if event.Sequence != int64(8+index) {
			t.Fatalf("event %d out of order: %+v", index, event)
		}
	}
}

// TestEventsAcceptsAnEmptyTailAndAFullPage pins the two honest edges: a run
// with nothing new answers an empty page at the same cursor, and a full page
// is allowed to say there is more.
func TestEventsAcceptsAnEmptyTailAndAFullPage(t *testing.T) {
	runID := "00000000-0000-7000-8000-00000000000a"
	t.Run("empty tail", func(t *testing.T) {
		client, _ := servingEvents(t, func(string, string) store.RunEventPage {
			return store.RunEventPage{RunID: runID, NextAfter: 12}
		})
		page, err := client.Events(context.Background(), runID, 12, 5)
		if err != nil {
			t.Fatal(err)
		}
		if len(page.Events) != 0 || page.NextAfter != 12 || page.HasMore {
			t.Fatalf("unexpected tail: %+v", page)
		}
	})
	t.Run("full page with more", func(t *testing.T) {
		client, _ := servingEvents(t, func(string, string) store.RunEventPage {
			return eventPage(runID, 0, 4, true)
		})
		page, err := client.Events(context.Background(), runID, 0, 4)
		if err != nil {
			t.Fatal(err)
		}
		if len(page.Events) != 4 || !page.HasMore || page.NextAfter != 4 {
			t.Fatalf("unexpected full page: %+v", page)
		}
	})
}

// TestEventsRefusesAPageThatDoesNotAnswerTheRequest pins the judgements made
// on the answer itself. Every one of these is a server that replied 200: the
// client refuses them anyway, because a caller walking a cursor it was handed
// would otherwise skip events it never saw and never know.
func TestEventsRefusesAPageThatDoesNotAnswerTheRequest(t *testing.T) {
	runID := "00000000-0000-7000-8000-00000000000a"
	other := "00000000-0000-7000-8000-00000000000b"
	sound := func() store.RunEventPage { return eventPage(runID, 0, 2, false) }
	for name, testCase := range map[string]struct {
		page   func() store.RunEventPage
		reason string
	}{
		"another run's page": {func() store.RunEventPage {
			page := sound()
			page.RunID = other
			return page
		}, "does not match request"},
		"more events than asked for": {func() store.RunEventPage {
			return eventPage(runID, 0, 3, false)
		}, "does not match request"},
		"cursor walks backwards": {func() store.RunEventPage {
			page := sound()
			page.NextAfter = -1
			return page
		}, "does not match request"},
		"cursor jumps past the page": {func() store.RunEventPage {
			page := eventPage(runID, 0, 1, false)
			page.NextAfter = 9
			return page
		}, "does not match request"},
		"more promised on a short page": {func() store.RunEventPage {
			page := eventPage(runID, 0, 1, true)
			return page
		}, "does not match request"},
		"the first event is not the one after the cursor": {func() store.RunEventPage {
			page := sound()
			page.Events[0].Sequence = 2
			return page
		}, "invalid event"},
		"an event with no identifier": {func() store.RunEventPage {
			page := sound()
			page.Events[0].ID = "not-an-id"
			return page
		}, "invalid event"},
		"an event with no kind": {func() store.RunEventPage {
			page := sound()
			page.Events[0].Kind = ""
			return page
		}, "invalid event"},
		"an event with no clock": {func() store.RunEventPage {
			page := sound()
			page.Events[0].HappenedAt = time.Time{}
			return page
		}, "invalid event"},
		"event data that is not an object": {func() store.RunEventPage {
			page := sound()
			page.Events[0].Data = json.RawMessage(`["running"]`)
			return page
		}, "invalid event"},
		"event data that is JSON null": {func() store.RunEventPage {
			page := sound()
			page.Events[0].Data = json.RawMessage(`null`)
			return page
		}, "invalid event"},
		"cursor behind its own last event": {func() store.RunEventPage {
			page := sound()
			page.NextAfter = 1
			return page
		}, "inconsistent"},
	} {
		t.Run(name, func(t *testing.T) {
			answer := testCase.page()
			client, _ := servingEvents(t, func(string, string) store.RunEventPage { return answer })
			page, err := client.Events(context.Background(), runID, 0, 2)
			if err == nil {
				t.Fatalf("dishonest page accepted: %+v", page)
			}
			if !strings.Contains(err.Error(), testCase.reason) {
				t.Fatalf("refusal %q does not name %q", err, testCase.reason)
			}
			if len(page.Events) != 0 || page.RunID != "" {
				t.Fatalf("a refused page was still handed back: %+v", page)
			}
		})
	}
}

// TestEventsCarriesTheServerRefusalBack pins that a non-2xx answer is reported
// as the status it was, not flattened into an empty page a caller would read
// as a run with nothing to say.
func TestEventsCarriesTheServerRefusalBack(t *testing.T) {
	runID := "00000000-0000-7000-8000-00000000000a"
	server := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		w.WriteHeader(http.StatusForbidden)
	}))
	defer server.Close()
	page, err := testClient(server).Events(context.Background(), runID, 0, 5)
	if err == nil || !strings.Contains(err.Error(), "HTTP 403") {
		t.Fatalf("server refusal not carried back: %v", err)
	}
	if len(page.Events) != 0 {
		t.Fatalf("a refused request answered events: %+v", page)
	}
}
