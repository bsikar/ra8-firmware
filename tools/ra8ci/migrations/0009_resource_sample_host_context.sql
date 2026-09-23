ALTER TABLE resource_samples
    ADD COLUMN host_os text CHECK (host_os IN ('linux', 'windows')),
    ADD COLUMN host_load_kind text CHECK (host_load_kind IN ('linux_load1', 'cpu_busy_equivalent'));

ALTER TABLE resource_samples
    ADD CONSTRAINT resource_samples_host_context_check CHECK (
        (host_os IS NULL AND host_load_kind IS NULL) OR
        (host_os = 'linux' AND host_load_kind = 'linux_load1') OR
        (host_os = 'windows' AND host_load_kind = 'cpu_busy_equivalent')
    );

CREATE INDEX resource_samples_host_context_idx ON resource_samples (host_os, host_load_kind, attempt_id);
