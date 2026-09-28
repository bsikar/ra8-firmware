// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package proxmox

import (
	"context"
	"errors"
	"net/http"
	"strings"
	"testing"
)

// A reservation's configuration is the last place a guest can drift away from
// what was reviewed, and the drift that matters is not the guest going
// missing: it is a guest that still answers to the right ID while carrying a
// second interface onto the management bridge, or no disk on approved storage
// at all. The checks below all run against a configuration the cluster listing
// already accepted.

// reservedConfig is the smallest configuration a reservation can carry and
// still be the guest that was reviewed: no lock, no protection flag, no
// template flag, one disk on approved storage, one interface on a reviewed
// bridge.
func reservedConfig(fields ...string) string {
	config := []string{
		`"name":"` + testIdentity.Name + `"`,
		`"description":"` + testIdentity.marker() + `"`,
		`"digest":"` + testDigest + `"`,
		`"scsi0":"ra8-tf-lab:vm-9000-disk-0,size=32G"`,
		`"net0":"virtio=AA:BB:CC:DD:EE:01,bridge=vmbr8,firewall=1"`,
	}
	return `{` + strings.Join(append(config, fields...), ",") + `}`
}

// reservedLab answers the three reads inspect makes: the cluster listing, the
// guest's configuration, and its live status.
func reservedLab(config string, status string, statusCode int) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/api2/json/cluster/resources":
			w.Header().Set("Content-Type", jsonType)
			_, _ = w.Write([]byte(`{"data":[` + guestRecord(9000, testIdentity.Name) + `]}`))
		case "/api2/json/nodes/pve/qemu/9000/config":
			w.Header().Set("Content-Type", jsonType)
			_, _ = w.Write([]byte(`{"data":` + config + `}`))
		case "/api2/json/nodes/pve/qemu/9000/status/current":
			if statusCode != 0 {
				w.WriteHeader(statusCode)
				return
			}
			w.Header().Set("Content-Type", jsonType)
			_, _ = w.Write([]byte(`{"data":{"vmid":9000,"status":"` + status + `"}}`))
		default:
			w.WriteHeader(http.StatusNotFound)
		}
	}
}

// The smallest honest configuration is accepted, and the fields Proxmox omits
// when they are off (lock, protection, template) are read as absent rather
// than as an unreadable answer.
func TestAReservationCarryingOnlyWhatItMustIsAccepted(t *testing.T) {
	client := clientAnswering(t, reservedLab(reservedConfig(), "stopped", 0))

	vm, err := client.Get(context.Background(), testIdentity)
	if err != nil {
		t.Fatalf("the smallest honest configuration was refused: %v", err)
	}
	if vm.Status != "stopped" || vm.ConfigDigest != testDigest {
		t.Fatalf("read back %+v, want a stopped guest at the reviewed digest", vm)
	}
	if vm.Locked || vm.Protected {
		t.Fatalf("absent lock and protection fields were read as set: %+v", vm)
	}
}

// Every interface has to sit on a reviewed bridge, not merely one of them: a
// second interface on the management bridge is exactly the escape the pool,
// storage and marker checks cannot see.
func TestAReservationIsRefusedOverTheNetworkItActuallyCarries(t *testing.T) {
	for _, attempt := range []struct {
		name   string
		config string
		says   string
	}{
		{"no interface at all",
			`{"name":"` + testIdentity.Name + `","description":"` + testIdentity.marker() + `","digest":"` + testDigest + `","scsi0":"ra8-tf-lab:vm-9000-disk-0,size=32G"}`,
			"no interface on an approved bridge"},
		{"an interface that declares no bridge",
			reservedConfig(`"net1":"virtio=AA:BB:CC:DD:EE:02,firewall=1"`),
			"declares no bridge"},
		{"a second interface on an unreviewed bridge",
			reservedConfig(`"net1":"virtio=AA:BB:CC:DD:EE:02,bridge=vmbr0,firewall=1"`),
			`unreviewed bridge "vmbr0"`},
		{"an interface whose bridge is empty",
			reservedConfig(`"net1":"virtio=AA:BB:CC:DD:EE:02,bridge=,firewall=1"`),
			"declares no bridge"},
	} {
		t.Run(attempt.name, func(t *testing.T) {
			client := clientAnswering(t, reservedLab(attempt.config, "stopped", 0))

			vm, err := client.Get(context.Background(), testIdentity)
			if !errors.Is(err, ErrConflict) {
				t.Fatalf("error = %v, want a conflict", err)
			}
			if !strings.Contains(err.Error(), attempt.says) {
				t.Fatalf("the refusal read %v, wanted it to name %q", err, attempt.says)
			}
			if !strings.Contains(err.Error(), "reservation") {
				t.Fatalf("the refusal %v does not say whose interface it is about", err)
			}
			if vm.Status != "" {
				t.Fatalf("a refused read still described a guest: %+v", vm)
			}
		})
	}
}

// The live status is the last read, and the one place the cluster can
// contradict itself: a guest the listing calls stopped and the status call
// calls something else is not a guest this client will report on.
func TestAReservationIsRefusedOverAStatusThatDoesNotHold(t *testing.T) {
	for _, attempt := range []struct {
		name   string
		status string
		code   int
		want   error
	}{
		{"the status cannot be read at all", "", http.StatusInternalServerError, ErrUnavailable},
		{"a status the listing contradicts", "running", 0, ErrProtocol},
		{"a status neither running nor stopped", "paused", 0, ErrProtocol},
		{"a status call answering about another guest", "stopped", 0, ErrProtocol},
	} {
		t.Run(attempt.name, func(t *testing.T) {
			serve := reservedLab(reservedConfig(), attempt.status, attempt.code)
			if attempt.name == "a status call answering about another guest" {
				serve = func(w http.ResponseWriter, r *http.Request) {
					if r.URL.Path == "/api2/json/nodes/pve/qemu/9000/status/current" {
						w.Header().Set("Content-Type", jsonType)
						_, _ = w.Write([]byte(`{"data":{"vmid":9199,"status":"stopped"}}`))
						return
					}
					reservedLab(reservedConfig(), "stopped", 0)(w, r)
				}
			}
			client := clientAnswering(t, serve)

			vm, err := client.Get(context.Background(), testIdentity)
			if !errors.Is(err, attempt.want) {
				t.Fatalf("error = %v, want %v", err, attempt.want)
			}
			if vm.Status != "" {
				t.Fatalf("a refused read still described a guest: %+v", vm)
			}
		})
	}
}
