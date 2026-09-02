-- offline-recorded-at
--
-- Client-supplied capture time for rows the firmware buffered to its SD card
-- during a connectivity outage and replayed later (see
-- docs/superpowers/specs/2026-09-02-offline-autonomy-design.md).
--
-- Without this, every backfilled row lands with created_at = now() (the server
-- default), collapsing a multi-hour outage into the reconnect instant. The
-- firmware sends recorded_at on every telemetry insert, live and replayed.
-- When null (older firmware), treat created_at as the capture time.

alter table public.sensor_metrics add column if not exists recorded_at timestamptz;
alter table public.activity_log  add column if not exists recorded_at timestamptz;
alter table public.relay_metrics add column if not exists recorded_at timestamptz;

-- A plain nullable column needs no RLS policy change (the firmware inserts with
-- the same anon/service key it uses today). After applying, reload the
-- PostgREST schema cache so the column is accepted on insert:
--   notify pgrst, 'reload schema';
