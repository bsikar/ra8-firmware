// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

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

// A CLI on a laptop never opens PostgreSQL: it asks the plane over mutual TLS
// and believes only an answer that matches the question it asked. Everything
// below is that second half. A report client that trusted the body it was
// handed would let a plane on the wrong repository, or a stale window, read
// back as this repository's numbers.

// reportMaterial is one authority, one server identity signed by it, and one
// operator identity signed by it, written where fetchSlowReport can read them.
type reportMaterial struct {
	serverIdentity tls.Certificate
	clientPool     *x509.CertPool
	caPath         string
	certPath       string
	keyPath        string
}

func mintReportMaterial(t *testing.T) reportMaterial {
	t.Helper()
	dir := t.TempDir()
	now := time.Now()

	caKey, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	caTemplate := &x509.Certificate{SerialNumber: big.NewInt(1),
		Subject: pkix.Name{CommonName: "report authority"}, NotBefore: now.Add(-time.Hour),
		NotAfter: now.Add(time.Hour), IsCA: true, BasicConstraintsValid: true,
		KeyUsage: x509.KeyUsageCertSign | x509.KeyUsageCRLSign}
	caDER, err := x509.CreateCertificate(rand.Reader, caTemplate, caTemplate, &caKey.PublicKey, caKey)
	if err != nil {
		t.Fatal(err)
	}
	ca, err := x509.ParseCertificate(caDER)
	if err != nil {
		t.Fatal(err)
	}
	caPEM := pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: caDER})
	caPath := filepath.Join(dir, "ca.pem")
	if err := os.WriteFile(caPath, caPEM, 0o600); err != nil {
		t.Fatal(err)
	}
	pool := x509.NewCertPool()
	if !pool.AppendCertsFromPEM(caPEM) {
		t.Fatal("the minted authority did not load into a pool")
	}

	leaf := func(name string, server bool) tls.Certificate {
		t.Helper()
		key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
		if err != nil {
			t.Fatal(err)
		}
		template := &x509.Certificate{SerialNumber: big.NewInt(2), NotBefore: now.Add(-time.Hour),
			NotAfter: now.Add(time.Hour), KeyUsage: x509.KeyUsageDigitalSignature}
		if server {
			template.Subject = pkix.Name{CommonName: "localhost"}
			template.DNSNames = []string{"localhost"}
			template.IPAddresses = []net.IP{net.ParseIP("127.0.0.1"), net.ParseIP("::1")}
			template.ExtKeyUsage = []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth}
		} else {
			template.Subject = pkix.Name{CommonName: "report operator"}
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
		certPath := filepath.Join(dir, name+"-cert.pem")
		keyPath := filepath.Join(dir, name+"-key.pem")
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
		return pair
	}

	serverIdentity := leaf("server", true)
	_ = leaf("operator", false)
	return reportMaterial{serverIdentity: serverIdentity, clientPool: pool, caPath: caPath,
		certPath: filepath.Join(dir, "operator-cert.pem"), keyPath: filepath.Join(dir, "operator-key.pem")}
}

// servingReport starts a mutual-TLS plane answering /v1/reports/slow with
// whatever the case supplies, and points the environment at it.
func servingReport(t *testing.T, material reportMaterial, handler http.HandlerFunc) *httptest.Server {
	t.Helper()
	server := httptest.NewUnstartedServer(handler)
	server.TLS = &tls.Config{MinVersion: tls.VersionTLS13,
		Certificates: []tls.Certificate{material.serverIdentity},
		ClientAuth:   tls.RequireAndVerifyClientCert, ClientCAs: material.clientPool}
	server.StartTLS()
	t.Cleanup(server.Close)
	bindReportEnvironment(t, material, server.URL)
	return server
}

func bindReportEnvironment(t *testing.T, material reportMaterial, serverURL string) {
	t.Helper()
	t.Setenv(envServerURL, serverURL)
	t.Setenv(envServerCA, material.caPath)
	t.Setenv(roleOperator.certEnv, material.certPath)
	t.Setenv(roleOperator.keyEnv, material.keyPath)
}

func soundSlowBody(t *testing.T, repository string, window time.Duration, tasks []store.SlowTask) []byte {
	t.Helper()
	encoded, err := json.Marshal(slowReportPayload{Repository: repository,
		WindowSeconds: int64(window.Seconds()), Tasks: tasks})
	if err != nil {
		t.Fatal(err)
	}
	return encoded
}

// The whole exchange: the request carries the question as query parameters and
// the answer is returned only because it matches it.
func TestFetchSlowReportAsksOverMutualTLSAndReturnsAMatchingAnswer(t *testing.T) {
	material := mintReportMaterial(t)
	window := 48 * time.Hour
	var askedPath, askedQuery string
	servingReport(t, material, func(w http.ResponseWriter, r *http.Request) {
		askedPath, askedQuery = r.URL.Path, r.URL.RawQuery
		w.Write(soundSlowBody(t, "bsikar/ra8-firmware", window,
			[]store.SlowTask{{Name: "build-cross", Tier: "heavy", Samples: 9}}))
	})

	report, err := fetchSlowReport(context.Background(), "bsikar/ra8-firmware", window, 5)
	if err != nil {
		t.Fatalf("a sound report was refused: %v", err)
	}
	if askedPath != "/v1/reports/slow" {
		t.Fatalf("asked %q, want /v1/reports/slow", askedPath)
	}
	for _, want := range []string{"repository=bsikar%2Fra8-firmware", "window_seconds=172800", "limit=5"} {
		if !strings.Contains(askedQuery, want) {
			t.Fatalf("query %q does not carry %q", askedQuery, want)
		}
	}
	if len(report.Tasks) != 1 || report.Tasks[0].Name != "build-cross" {
		t.Fatalf("the answer did not come back whole: %+v", report)
	}
}

// RA8CI_SERVER_URL names an origin. Anything carrying a path, a query, a
// fragment, credentials, or a scheme that is not HTTPS is refused before a
// key is read, so a misconfigured deployment fails at the CLI rather than
// sending an operator identity somewhere unintended.
func TestFetchSlowReportRequiresAnHTTPSOrigin(t *testing.T) {
	material := mintReportMaterial(t)
	for name, endpoint := range map[string]string{
		"plain HTTP":       "http://plane.example",
		"no host":          "https://",
		"a path":           "https://plane.example/v1",
		"a query":          "https://plane.example?tenant=2",
		"a fragment":       "https://plane.example#top",
		"credentials":      "https://operator@plane.example",
		"not a URL at all": "://",
	} {
		bindReportEnvironment(t, material, endpoint)
		_, err := fetchSlowReport(context.Background(), "bsikar/ra8-firmware", time.Hour, 5)
		if err == nil || !strings.Contains(err.Error(), "must be an HTTPS origin") {
			t.Fatalf("%s (%q) was not refused as an origin: %v", name, endpoint, err)
		}
	}
}

// A bare origin with a trailing slash is the same origin, and is accepted.
func TestFetchSlowReportAcceptsAnOriginWithATrailingSlash(t *testing.T) {
	material := mintReportMaterial(t)
	server := servingReport(t, material, func(w http.ResponseWriter, r *http.Request) {
		w.Write(soundSlowBody(t, "bsikar/ra8-firmware", time.Hour, nil))
	})
	bindReportEnvironment(t, material, server.URL+"/")
	if _, err := fetchSlowReport(context.Background(), "bsikar/ra8-firmware", time.Hour, 5); err != nil {
		t.Fatalf("a trailing slash was refused: %v", err)
	}
}

// Each piece of trust material is named in its own refusal, so an operator
// knows which file to look at rather than being told the connection failed.
func TestFetchSlowReportNamesTheTrustMaterialItCannotUse(t *testing.T) {
	t.Run("an absent server CA", func(t *testing.T) {
		material := mintReportMaterial(t)
		bindReportEnvironment(t, material, "https://plane.example")
		t.Setenv(envServerCA, filepath.Join(t.TempDir(), "absent.pem"))
		_, err := fetchSlowReport(context.Background(), "r", time.Hour, 5)
		if err == nil || !strings.Contains(err.Error(), "read server CA") {
			t.Fatalf("an absent CA was not named: %v", err)
		}
	})
	t.Run("a server CA that is not a certificate", func(t *testing.T) {
		material := mintReportMaterial(t)
		bindReportEnvironment(t, material, "https://plane.example")
		path := filepath.Join(t.TempDir(), "not-a-ca.pem")
		if err := os.WriteFile(path, []byte("not a certificate"), 0o600); err != nil {
			t.Fatal(err)
		}
		t.Setenv(envServerCA, path)
		_, err := fetchSlowReport(context.Background(), "r", time.Hour, 5)
		if err == nil || !strings.Contains(err.Error(), "report server trust") {
			t.Fatalf("an unusable CA was not named: %v", err)
		}
	})
	t.Run("an absent operator identity", func(t *testing.T) {
		material := mintReportMaterial(t)
		bindReportEnvironment(t, material, "https://plane.example")
		t.Setenv(roleOperator.certEnv, filepath.Join(t.TempDir(), "absent-cert.pem"))
		_, err := fetchSlowReport(context.Background(), "r", time.Hour, 5)
		if err == nil || !strings.Contains(err.Error(), "report client identity") {
			t.Fatalf("an absent identity was not named: %v", err)
		}
	})
}

// The environment has to be complete before anything is read at all.
func TestFetchSlowReportRefusesAnIncompleteEnvironment(t *testing.T) {
	material := mintReportMaterial(t)
	bindReportEnvironment(t, material, "https://plane.example")
	t.Setenv(envServerURL, "")
	_, err := fetchSlowReport(context.Background(), "r", time.Hour, 5)
	if err == nil || !strings.Contains(err.Error(), envServerURL) {
		t.Fatalf("a missing server URL was not named: %v", err)
	}
}

// Anything but 200 is reported with the status, rather than being decoded as
// though the body were a report.
func TestFetchSlowReportReportsTheStatusItWasAnswered(t *testing.T) {
	material := mintReportMaterial(t)
	servingReport(t, material, func(w http.ResponseWriter, r *http.Request) {
		http.Error(w, "nope", http.StatusForbidden)
	})
	_, err := fetchSlowReport(context.Background(), "bsikar/ra8-firmware", time.Hour, 5)
	if err == nil || !strings.Contains(err.Error(), "HTTP 403") {
		t.Fatalf("a 403 was not reported as one: %v", err)
	}
}

// A redirect is an answer, not a hop to follow: an operator identity is not
// re-presented wherever a plane points.
func TestFetchSlowReportDoesNotFollowARedirect(t *testing.T) {
	material := mintReportMaterial(t)
	servingReport(t, material, func(w http.ResponseWriter, r *http.Request) {
		http.Redirect(w, r, "https://elsewhere.example/v1/reports/slow", http.StatusFound)
	})
	_, err := fetchSlowReport(context.Background(), "bsikar/ra8-firmware", time.Hour, 5)
	if err == nil || !strings.Contains(err.Error(), "HTTP 302") {
		t.Fatalf("a redirect was followed rather than reported: %v", err)
	}
}

// The body is read under a one-megabyte limit, so a plane that floods the
// client is refused on the size rather than on whatever it eventually parses.
func TestFetchSlowReportRefusesABodyPastItsLimit(t *testing.T) {
	material := mintReportMaterial(t)
	servingReport(t, material, func(w http.ResponseWriter, r *http.Request) {
		w.Write(make([]byte, (1<<20)+1))
	})
	_, err := fetchSlowReport(context.Background(), "bsikar/ra8-firmware", time.Hour, 5)
	if err == nil || !strings.Contains(err.Error(), "exceeds response limit") {
		t.Fatalf("an oversized body was not refused on its size: %v", err)
	}
}

// An answer is believed only when it answers the question that was asked. A
// different repository, a different window, or more rows than were requested
// all mean the client is reading someone else's report.
func TestFetchSlowReportRefusesAnAnswerToADifferentQuestion(t *testing.T) {
	window := time.Hour
	for name, body := range map[string]func(t *testing.T) []byte{
		"another repository": func(t *testing.T) []byte {
			return soundSlowBody(t, "someone/else", window, nil)
		},
		"another window": func(t *testing.T) []byte {
			return soundSlowBody(t, "bsikar/ra8-firmware", 72*time.Hour, nil)
		},
		"more rows than were asked for": func(t *testing.T) []byte {
			return soundSlowBody(t, "bsikar/ra8-firmware", window,
				[]store.SlowTask{{Name: "a"}, {Name: "b"}, {Name: "c"}})
		},
		"a second document after the first": func(t *testing.T) []byte {
			first := soundSlowBody(t, "bsikar/ra8-firmware", window, nil)
			return append(first, first...)
		},
	} {
		t.Run(name, func(t *testing.T) {
			material := mintReportMaterial(t)
			answer := body(t)
			servingReport(t, material, func(w http.ResponseWriter, r *http.Request) {
				w.Write(answer)
			})
			_, err := fetchSlowReport(context.Background(), "bsikar/ra8-firmware", window, 2)
			if err == nil || !strings.Contains(err.Error(), "does not match request") {
				t.Fatalf("%s was accepted: %v", name, err)
			}
		})
	}
}

// A field the client does not know is a plane speaking a newer protocol, and
// is refused rather than silently dropped.
func TestFetchSlowReportRefusesAFieldItDoesNotKnow(t *testing.T) {
	material := mintReportMaterial(t)
	servingReport(t, material, func(w http.ResponseWriter, r *http.Request) {
		w.Write([]byte(`{"repository":"bsikar/ra8-firmware","window_seconds":3600,"tasks":[],"surprise":1}`))
	})
	_, err := fetchSlowReport(context.Background(), "bsikar/ra8-firmware", time.Hour, 5)
	if err == nil || !strings.Contains(err.Error(), "decode slow report") {
		t.Fatalf("an unknown field was not refused: %v", err)
	}
}

// A cancelled context stops the exchange rather than the client waiting out
// its own thirty-second timeout.
func TestFetchSlowReportStopsOnACancelledContext(t *testing.T) {
	material := mintReportMaterial(t)
	servingReport(t, material, func(w http.ResponseWriter, r *http.Request) {
		w.Write(soundSlowBody(t, "bsikar/ra8-firmware", time.Hour, nil))
	})
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if _, err := fetchSlowReport(ctx, "bsikar/ra8-firmware", time.Hour, 5); err == nil {
		t.Fatal("a cancelled exchange still returned a report")
	}
}
