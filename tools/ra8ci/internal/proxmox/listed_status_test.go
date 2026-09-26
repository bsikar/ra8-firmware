// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package proxmox

import (
	"context"
	"errors"
	"strings"
	"testing"
)

// unactionableStatuses are states Proxmox really reports and this client has
// no operation for: a paused or suspended guest, and the "unknown" a cluster
// record carries when the node holding the guest has stopped answering.
var unactionableStatuses = []string{"paused", "suspended", "unknown", "prelaunch"}

func TestListRefusesAGuestInAStatusThisClientCannotActOn(t *testing.T) {
	for _, status := range unactionableStatuses {
		listed, err := listWith(t, map[string]any{"status": status})
		if !errors.Is(err, ErrProtocol) {
			t.Fatalf("status %q listed as an actionable guest: %+v, %v", status, listed, err)
		}
		if listed != nil {
			t.Fatalf("refused list still reported guests for status %q: %+v", status, listed)
		}
	}
}

func TestTheRefusalNamesTheVMIDAndTheStatus(t *testing.T) {
	_, err := listWith(t, map[string]any{"status": "paused"})
	if err == nil || !strings.Contains(err.Error(), "9000") || !strings.Contains(err.Error(), `"paused"`) {
		t.Fatalf("refusal does not name the guest and its status: %v", err)
	}
}

func TestListStillReportsAGuestInAStatusThisClientActsOn(t *testing.T) {
	for _, status := range []string{"running", "stopped"} {
		listed, err := listWith(t, map[string]any{"status": status})
		if err != nil || len(listed) != 1 || listed[0].Status != status {
			t.Fatalf("status %q not reported: %+v, %v", status, listed, err)
		}
	}
}

// A cluster record that omits the field is stating nothing, and inspect
// tolerates the same omission when it compares the record with status/current.
func TestAnUnstatedStatusIsLeftAlone(t *testing.T) {
	listed, err := listWith(t, map[string]any{"status": ""})
	if err != nil || len(listed) != 1 || listed[0].Status != "" {
		t.Fatalf("unstated status refused or invented: %+v, %v", listed, err)
	}
}

// The listing and the reservation read are the same client reading the same
// guest, so a stated status they disagree about would be a status one path
// acts on and the other refuses.
func TestTheListingAndTheReservationJudgeAStatedStatusAlike(t *testing.T) {
	for _, status := range append([]string{"running", "stopped"}, unactionableStatuses...) {
		f := newFake()
		f.exists = true
		f.status = status
		client, _ := testClient(t, f)
		_, listErr := client.List(context.Background())
		_, getErr := client.Get(context.Background(), testIdentity)
		if (listErr == nil) != (getErr == nil) {
			t.Fatalf("status %q: listing says %v, reservation read says %v", status, listErr, getErr)
		}
	}
}

// An identity conflict is the more serious finding and must not be masked by
// whatever state the wrong guest happens to be in.
func TestAnIdentityConflictIsReportedBeforeAnUnactionableStatus(t *testing.T) {
	_, err := listWith(t, map[string]any{"node": "pve2", "status": "paused"})
	if !errors.Is(err, ErrConflict) {
		t.Fatalf("status refusal masked the identity conflict: %v", err)
	}
}

// The status rule reads the cluster record and nothing else, so it cannot
// depend on a guest's configuration being readable.
func TestTheStatusRuleIsDecidedOnTheClusterRecordAlone(t *testing.T) {
	f := newFake()
	f.exists = true
	f.status = "unknown"
	f.marker = "RA8CI_RESERVATION=someone-else"
	client, _ := testClient(t, f)
	if _, err := client.List(context.Background()); !errors.Is(err, ErrProtocol) {
		t.Fatalf("status refusal needed a readable configuration: %v", err)
	}
}
