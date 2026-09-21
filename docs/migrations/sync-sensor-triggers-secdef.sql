-- sync-sensor-triggers-secdef
--
-- APPLIED to prod (project qkqeysggrqhxizkdmbhx / "SF-500") on 2026-09-07
-- as migration `sync_sensor_triggers_security_definer`.
--
-- activity_log has four AFTER INSERT triggers. Two (handle_auto_dosing_alarm,
-- dispatch_alarm_notification) were already SECURITY DEFINER; the sensor-sync
-- pair was not. sync_direct_sensors() / sync_lora_sensors() parse
-- "Sensor init: EC(ID=3) ..." / "LoRa scan: ..." rows and upsert into
-- sensor_config. sensor_config's RLS only allows role `authenticated`, so when
-- the firmware (role: anon) inserts a "Sensor init" row, the trigger's write to
-- sensor_config is denied, the whole activity_log INSERT rolls back, and
-- PostgREST returns HTTP 401 / SQLSTATE 42501.
--
-- Card-less that failed row is just dropped. With the offline-autonomy SD
-- journal it is a poison pill: backfill.cpp replays the journal oldest-first and
-- (before the companion firmware fix) stopped on the first non-2xx, so this one
-- row blocked every telemetry row behind it forever and pending_b grew
-- unbounded. Verified on sf500_3a387c 2026-09-07.
--
-- Both functions already `SET search_path TO 'public'`, so SECURITY DEFINER is
-- safe here. Side effect: the sensor_config.is_active sync from "Sensor init" /
-- "LoRa scan" rows starts working fleet-wide (it never has). Only consumer of
-- sensor_config.is_active is the admin sensor list (AdminDevicePanel.tsx).

alter function public.sync_direct_sensors() security definer;
alter function public.sync_lora_sensors()  security definer;
