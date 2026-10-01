-- A completed runner may be stopped while cleanup continues. Preserve the
-- drain intent so a replayed Assigned event can never restart that VM.
ALTER TABLE runner_vms ADD COLUMN cleanup_requested boolean NOT NULL DEFAULT false;
