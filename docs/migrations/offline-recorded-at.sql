-- offline-recorded-at
--
-- APPLIED to prod (project qkqeysggrqhxizkdmbhx / "SF-500") on 2026-09-03
-- as migration `offline_recorded_at`. All three columns present, nullable.
--
-- Client-supplied capture time for rows the firmware buffered to its SD card
-- during a connectivity outage and replayed later (see
-- docs/superpowers/specs/2026-09-02-offline-autonomy-design.md).
--
-- Without this, every backfilled row lands with the server-default timestamp
-- (activity_log.created_at, sensor_metrics.updated_at, relay_metrics.updated_at
-- all default now()), collapsing a multi-hour outage into the reconnect instant.
-- The firmware sends recorded_at on every telemetry insert, live and replayed.
-- When null (older firmware), treat the server timestamp as the capture time.

alter table public.sensor_metrics add column if not exists recorded_at timestamptz;
alter table public.activity_log  add column if not exists recorded_at timestamptz;
alter table public.relay_metrics add column if not exists recorded_at timestamptz;

-- A plain nullable column needs no RLS policy change (the firmware inserts with
-- the same anon/service key it uses today). After applying, reload the
-- PostgREST schema cache so the column is accepted on insert:
--   notify pgrst, 'reload schema';
