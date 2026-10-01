// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package proxmox

import (
	"context"
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

// Everything this client believes about the lab arrives through one request
// path, so what that path refuses is what keeps a bad answer from being read
// as a fact about a guest. Each refusal below has to reach the caller as its
// own sentinel: an operator who sees "access denied" reaches for the token,
// and one who sees "invalid response" reaches for the endpoint in front of
// Proxmox. Collapsing them into one error would send both to the wrong place.

// clientAnswering builds a client pointed at a server that answers however the
// case wants, which the shared fake cannot do: it only speaks well-formed
// Proxmox.
func clientAnswering(t *testing.T, answer http.HandlerFunc) *Client {
	t.Helper()
	server := httptest.NewTLSServer(answer)
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
	client, err := New(Config{Endpoint: server.URL, CAFile: ca, TokenFile: token,
		Node: "pve", Pool: "ra8-tf-lab", Storage: "ra8-tf-lab",
		AllowedVMIDs: []int{9000}, TemplateVMIDs: []int{9001}, Bridges: []string{"vmbr8", "vmbr9"},
		RequestTimeout: time.Second, OperationTimeout: time.Second, TaskPollInterval: time.Millisecond})
	if err != nil {
		t.Fatal(err)
	}
	return client
}

// answering writes a status, a content type and a body exactly as given, so a
// case can say something Proxmox never would.
func answering(status int, contentType, body string) http.HandlerFunc {
	return func(w http.ResponseWriter, _ *http.Request) {
		if contentType != "" {
			w.Header().Set("Content-Type", contentType)
		}
		w.WriteHeader(status)
		_, _ = w.Write([]byte(body))
	}
}

const jsonType = "application/json"

// What the endpoint says, and what the caller is told it means.
func TestEachRefusalReachesTheCallerAsItsOwnSentinel(t *testing.T) {
	oversized := `{"data":[` + strings.Repeat(`{"vmid":9000},`, 90000) + `{"vmid":9000}]}`
	for _, attempt := range []struct {
		name   string
		answer http.HandlerFunc
		want   error
	}{
		{"an unauthorized answer", answering(http.StatusUnauthorized, jsonType, `{"data":null}`), ErrDenied},
		{"a forbidden answer", answering(http.StatusForbidden, jsonType, `{"data":null}`), ErrDenied},
		{"nothing at that path", answering(http.StatusNotFound, jsonType, `{"data":null}`), ErrNotFound},
		{"a server fault", answering(http.StatusInternalServerError, jsonType, `{"data":[]}`), ErrUnavailable},
		{"a gateway in the way", answering(http.StatusBadGateway, jsonType, `{"data":[]}`), ErrUnavailable},
		{"HTML where JSON was asked for", answering(http.StatusOK, "text/html", `<html>login</html>`), ErrProtocol},
		{"JSON with no content type at all", answering(http.StatusOK, "", `{"data":[]}`), ErrProtocol},
		{"a body past the readable bound", answering(http.StatusOK, jsonType, oversized), ErrProtocol},
		{"an envelope with no data", answering(http.StatusOK, jsonType, `{}`), ErrProtocol},
		{"an envelope whose data is null", answering(http.StatusOK, jsonType, `{"data":null}`), ErrProtocol},
		{"an envelope carrying errors", answering(http.StatusOK, jsonType, `{"data":[],"errors":{"vmid":"bad"}}`), ErrProtocol},
		{"a second document after the envelope", answering(http.StatusOK, jsonType, `{"data":[]}{"data":[]}`), ErrProtocol},
		{"data of the wrong shape", answering(http.StatusOK, jsonType, `{"data":"nine thousand"}`), ErrProtocol},
		{"nothing at all", answering(http.StatusOK, jsonType, ``), ErrProtocol},
	} {
		t.Run(attempt.name, func(t *testing.T) {
			client := clientAnswering(t, attempt.answer)
			guests, err := client.List(context.Background())
			if !errors.Is(err, attempt.want) {
				t.Fatalf("error = %v, want %v", err, attempt.want)
			}
			if len(guests) != 0 {
				t.Fatalf("a refused answer still yielded guests: %+v", guests)
			}
		})
	}
}

// The sentinels are the whole point, so they must not be each other. A denied
// answer read as unavailable would have an operator waiting for the endpoint
// to come back when the token is what expired.
func TestTheRefusalsAreNotEachOther(t *testing.T) {
	denied := clientAnswering(t, answering(http.StatusForbidden, jsonType, `{"data":null}`))
	if _, err := denied.List(context.Background()); errors.Is(err, ErrUnavailable) || errors.Is(err, ErrProtocol) {
		t.Fatalf("a denied answer was reported as something else: %v", err)
	}
	faulted := clientAnswering(t, answering(http.StatusInternalServerError, jsonType, `{"data":[]}`))
	if _, err := faulted.List(context.Background()); errors.Is(err, ErrDenied) || errors.Is(err, ErrNotFound) {
		t.Fatalf("a server fault was reported as an access decision: %v", err)
	}
	protocol := clientAnswering(t, answering(http.StatusOK, "text/html", `<html>login</html>`))
	if _, listErr := protocol.List(context.Background()); errors.Is(listErr, ErrDenied) || errors.Is(listErr, ErrUnavailable) {
		t.Fatalf("a malformed answer was reported as an access or reachability problem: %v", listErr)
	}
}

// A fault names the status, which is the one detail that tells an operator
// which box in front of Proxmox answered.
func TestAServerFaultNamesItsStatus(t *testing.T) {
	client := clientAnswering(t, answering(http.StatusServiceUnavailable, jsonType, `{"data":[]}`))
	_, err := client.List(context.Background())
	if err == nil || !strings.Contains(err.Error(), "503") {
		t.Fatalf("error = %v, want the status named", err)
	}
}
