package store

import (
	"encoding/json"
	"testing"
)

func TestParseTerraformStateAcceptsTerraformUUIDLineage(t *testing.T) {
	state := map[string]any{
		"version": 4, "serial": 0,
		"lineage":           "2f5a13ed-f770-4f68-a246-52c1e8f7e018",
		"terraform_version": "1.10.5",
	}
	body, err := json.Marshal(state)
	if err != nil {
		t.Fatal(err)
	}
	header, digest, err := parseTerraformState(body)
	if err != nil || header.Lineage != state["lineage"] || len(digest) != 64 {
		t.Fatalf("valid Terraform UUID lineage rejected: header=%+v digest=%q err=%v", header, digest, err)
	}
	state["lineage"] = "not-a-uuid"
	body, _ = json.Marshal(state)
	if _, _, err := parseTerraformState(body); err == nil {
		t.Fatal("accepted malformed Terraform lineage")
	}
}
