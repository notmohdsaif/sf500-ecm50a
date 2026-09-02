-- cellular_apn: APN string for the SIM in this unit's onboard 4G modem.
-- Null/empty means cellular fallback stays inert even if the modem hardware
-- is detected (same implicit-by-presence pattern as tasmota_plug_host — no
-- separate enable flag). Admin sets this once per unit at deployment.
--
-- Applied to project qkqeysggrqhxizkdmbhx on 2026-09-02 (migration name: cellular_apn).

alter table device_management
  add column cellular_apn text;
