// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package proxmox

import (
	"context"
	"crypto/x509"
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

// Everything this client does begins with the cluster listing, so what the
// listing is allowed to say is the narrowest point in the whole package. A
// record the client cannot make sense of has to stop the read: carrying a
// half-understood listing forward would mean addressing a guest by an ID the
// cluster never agreed to.

// clientAllowing is clientAnswering with the allowlist opened up, so a listing
// holding more than one of our guests can be read back.
func clientAllowing(t *testing.T, allowed []int, answer http.HandlerFunc) *Client {
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
		AllowedVMIDs: allowed, TemplateVMIDs: []int{9001}, Bridges: []string{"vmbr8", "vmbr9"},
		RequestTimeout: time.Second, OperationTimeout: time.Second, TaskPollInterval: time.Millisecond})
	if err != nil {
		t.Fatal(err)
	}
	if _, err := x509.ParseCertificate(cert.Raw); err != nil {
		t.Fatal(err)
	}
	return client
}

// guestRecord is one cluster listing row for a guest of ours.
func guestRecord(vmid int, name string) string {
	return `{"vmid":` + itoaVMID(vmid) + `,"type":"qemu","node":"pve","name":"` + name + `","pool":"ra8-tf-lab","status":"stopped","template":0}`
}

func itoaVMID(vmid int) string {
	digits := ""
	for vmid > 0 {
		digits = string(rune('0'+vmid%10)) + digits
		vmid /= 10
	}
	if digits == "" {
		return "0"
	}
	return digits
}

// A listing comes back as a map, so nothing about the order the cluster sent
// it survives. The client sorts by VM ID, which is what makes a sweep's
// report stable between runs over an unchanged lab.
func TestAListingIsReportedInVMIDOrderHoweverItArrived(t *testing.T) {
	client := clientAllowing(t, []int{9000, 9002, 9004}, func(w http.ResponseWriter, _ *http.Request) {
		w.Header().Set("Content-Type", jsonType)
		_, _ = w.Write([]byte(`{"data":[` + strings.Join([]string{
			guestRecord(9004, "ra8-lab-c"),
			guestRecord(9000, "ra8-lab-a"),
			guestRecord(9002, "ra8-lab-b"),
		}, ",") + `]}`))
	})

	listed, err := client.List(context.Background())
	if err != nil {
		t.Fatalf("a well-formed listing was refused: %v", err)
	}
	if len(listed) != 3 {
		t.Fatalf("listed %d guests, want 3: %+v", len(listed), listed)
	}
	for index, want := range []int{9000, 9002, 9004} {
		if listed[index].Identity.VMID != want {
			t.Fatalf("listed[%d] is VM %d, want %d: %+v", index, listed[index].Identity.VMID, want, listed)
		}
	}
}

// Records the client cannot make sense of. None of these is about OUR guests:
// the refusal covers the whole listing, because a cluster that reports a VM ID
// outside the possible range, or the same ID twice, is one whose answers
// cannot be indexed at all.
func TestAListingThatCannotBeIndexedIsRefusedWhole(t *testing.T) {
	for _, attempt := range []struct {
		name string
		rows string
		says string
	}{
		{"a VM ID below the possible range", `{"vmid":99,"type":"qemu","node":"pve"}`, ""},
		{"a VM ID above the possible range", `{"vmid":1000000000,"type":"qemu","node":"pve"}`, ""},
		{"a record with no type at all", `{"vmid":9100,"type":"","node":"pve"}`, ""},
		{"the same VM ID twice", guestRecord(9100, "ra8-lab-a") + "," + guestRecord(9100, "ra8-lab-b"), "duplicate"},
	} {
		t.Run(attempt.name, func(t *testing.T) {
			client := clientAnswering(t, answering(http.StatusOK, jsonType, `{"data":[`+attempt.rows+`]}`))

			listed, err := client.List(context.Background())
			if !errors.Is(err, ErrProtocol) {
				t.Fatalf("error = %v, want a protocol refusal", err)
			}
			if attempt.says != "" && !strings.Contains(err.Error(), attempt.says) {
				t.Fatalf("the refusal read %v, wanted it to name %q", err, attempt.says)
			}
			if listed != nil {
				t.Fatalf("a refused listing still reported guests: %+v", listed)
			}
		})
	}
}

// A read of one guest fails wherever the answer fails: at the listing, at the
// guest's absence from it, or at its own configuration. Each is carried back
// as itself, since a caller decides very different things on "not there" and
// "cannot tell".
func TestAReadOfOneGuestCarriesBackWhereItFailed(t *testing.T) {
	ourGuest := guestRecord(9000, testIdentity.Name)

	for _, attempt := range []struct {
		name  string
		serve http.HandlerFunc
		want  error
	}{
		{"the listing cannot be read", answering(http.StatusInternalServerError, "", ""), ErrUnavailable},
		{"the guest is not in the listing", answering(http.StatusOK, jsonType, `{"data":[]}`), ErrNotFound},
		{"the guest's own configuration cannot be read", func(w http.ResponseWriter, r *http.Request) {
			if r.URL.Path == "/api2/json/cluster/resources" {
				w.Header().Set("Content-Type", jsonType)
				_, _ = w.Write([]byte(`{"data":[` + ourGuest + `]}`))
				return
			}
			w.WriteHeader(http.StatusServiceUnavailable)
		}, ErrUnavailable},
		{"the guest in the listing is not the one reserved", func(w http.ResponseWriter, _ *http.Request) {
			w.Header().Set("Content-Type", jsonType)
			_, _ = w.Write([]byte(`{"data":[` + strings.Replace(ourGuest, `"pool":"ra8-tf-lab"`, `"pool":"other"`, 1) + `]}`))
		}, ErrConflict},
	} {
		t.Run(attempt.name, func(t *testing.T) {
			client := clientAnswering(t, attempt.serve)

			vm, err := client.Get(context.Background(), testIdentity)
			if !errors.Is(err, attempt.want) {
				t.Fatalf("error = %v, want %v", err, attempt.want)
			}
			if vm.Identity.VMID != 0 || vm.Status != "" {
				t.Fatalf("a failed read still described a guest: %+v", vm)
			}
		})
	}
}
