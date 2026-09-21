# SF-500 microSD Provisioning

How to prepare and verify a microSD card for an SF-500 controller before it
ships, and how to confirm it is working after a customer installs it, without a
serial console.

The offline-autonomy firmware uses the card to keep dosing and water-in
detection running through a connectivity loss and to buffer telemetry for
replay when the link returns. A unit with no card, or with an unreadable card,
keeps working exactly as before (it posts live or drops when offline); it just
does not get the offline buffering.

---

## 1. Which card to buy

| Property | Requirement |
| --- | --- |
| Capacity | 8 GB is sufficient. Larger is fine, no benefit. |
| Grade | Industrial or high-endurance. The card is written continuously for the life of the unit. A consumer card will wear out. |
| Filesystem | FAT32. See section 2. The firmware mounts FAT16 and FAT32 only. It cannot mount exFAT, which is how many cards ship from the factory. |
| Interface | Standard microSDHC. UHS speed class is not important. |

Do not rely on a card being usable straight from its retail packaging. Prepare
every card as below.

---

## 2. Office preparation (once per card, on a computer)

You need a computer with an SD card reader.

### 2a. Format as FAT32

Use the SD Association SD Card Formatter tool (free, Windows and macOS). It
selects FAT32 for cards up to 32 GB and aligns the filesystem correctly.

1. Insert the card into the reader.
2. Open SD Card Formatter.
3. Select the card. Check the capacity shown matches the card label.
4. Choose "Overwrite format" for a new card, or "Quick format" for a card you
   have already verified once.
5. Format.

macOS command-line alternative (replace diskN with the card, check with
`diskutil list` first, and be certain of the disk number):

```
diskutil eraseDisk MS-DOS SF500 MBR /dev/diskN
```

Windows built-in Disk Management also works: delete the partition, create a new
one, format FAT32. For cards larger than 32 GB the built-in Windows formatter
will not offer FAT32, which is another reason to keep to 8 GB.

### 2b. Verify the card is real and healthy

Counterfeit cards are common. They report a false capacity and fail on write.
A dead or fake card is worse than no card.

Quick check, sufficient for most cases:

1. Copy a file of about 100 MB onto the formatted card.
2. Eject and reinsert the card.
3. Open the file and confirm it is intact, or compare its checksum.
4. Delete the file.

Batch check, for a new supplier or a suspect batch:

- Windows: H2testw, "write + verify" on the whole card.
- macOS or Linux: `f3write` then `f3read`.

Either tool writes the whole card and reads it back, confirming true capacity
and that every sector holds data. This takes time proportional to card size
(a few minutes for 8 GB).

### 2c. Handle and label

- The card can be left blank after formatting. The firmware creates the folders
  it needs (`/config`, `/state`, `/buffer`) on first mount.
- Keep cards in antistatic packaging until fitted.
- If cards are prepared in a batch, mark the batch and date so a bad supplier
  batch can be traced later.

---

## 3. Fitting the card

1. Power the unit off.
2. Insert the microSD card into the slot until it latches (a push-push slot
   clicks and holds; it should not spring back out).
3. Power the unit on.

The card can also be fitted with the unit running. The firmware detects it
within a few seconds and starts using it without a restart. Fitting it with the
power off is preferred for a clean first boot.

---

## 4. Confirming it works, without a serial console

The firmware reports the card state to the cloud and over MQTT. Check either.

### 4a. Supabase (activity log)

After the unit boots and comes online, a row appears in `activity_log` for the
device:

```
boot summary: sd=ok/7480MB cfg=loaded ecTarget=1.50 autoDosing=1 mixing=1 dosingTime=30 schedules=2 journalPendingB=0 clock=ntp
```

- `sd=ok/<N>MB` means the card mounted and has N MB free. Done.
- `sd=absent` means no card was detected.
- `sd=unreadable` means a card is present but did not mount. Almost always this
  is a card that was not formatted as FAT32, or a bad card. Re-run section 2.
- `cfg=loaded` means the unit found its saved configuration on the card and can
  keep dosing autonomously even if it boots with no connectivity. `cfg=none` is
  normal only on a unit's first boots, before it has been online once to fetch
  and save its config; after that it should read `cfg=loaded` on every boot.
- `journalPendingB=<N>` is the buffered-but-unsent byte count (same value as
  `pending_b` in section 4b). It is 0 or small on a healthy online unit.

### 4b. MQTT (live, no reboot needed)

Subscribe to the unit's data topic. The last six hex characters of the MAC are
in the device name (`sf500_107888` gives `107888`):

```
mosquitto_sub -h broker.emqx.io -p 1883 -t 'sf500/107888/data' -C 1
```

The payload contains an `sd` block:

```json
"sd": { "state": "ok", "free_mb": 7480, "pending_b": 0 }
```

- `state` is `ok`, `absent`, or `unreadable`, as above.
- `free_mb` is free space in MB.
- `pending_b` is the number of telemetry bytes buffered on the card and not yet
  sent to the cloud. It is 0 or small when the unit is online and caught up. It
  grows during a connectivity loss and drains back toward 0 when the link
  returns. A `pending_b` that only ever grows and never drains while the unit is
  online indicates a problem.

For a customer site with no MQTT client available, use 4a: ask the customer to
power cycle the unit, then check the `activity_log` boot summary row.

---

## 5. What can go wrong, and what to do

| Symptom | Cause | Action |
| --- | --- | --- |
| `sd=absent` with a card fitted | Card not latched, or slot damaged | Reseat the card firmly. If still absent, the slot or card is faulty. |
| `sd=unreadable` | Card is exFAT, unformatted, or corrupt | Re-run section 2. If it fails again on a known-good format, the card is bad. |
| `pending_b` grows while online and never drains | Backend or credentials issue, not the card | Check the unit is actually reaching Supabase (heartbeat in `device_management`). |
| `free_mb` unexpectedly low on a new card | Card was not fully formatted, or is a fake reporting false capacity | Re-run 2b. |

### Field recovery with a serial console (last resort)

If a unit is on the bench with a serial cable, an unreadable card can be
reformatted in place. At 9600 baud, type:

```
SDFORMAT CONFIRM
```

This erases the card and writes a fresh FAT32 filesystem. It takes a few
minutes and the unit continues dosing throughout. This is only for a bench with
serial access. The normal path is section 2 on a computer.

---

## 6. Firmware and rollout notes

- The card features need firmware from the `offline-autonomy` line. A unit on
  older firmware ignores any card fitted.
- The `recorded_at` database column must exist (migration `offline_recorded_at`,
  applied 2026-09-03). Without it, a unit on this firmware cannot log to
  Supabase at all.
- Rolling this firmware to units that will not have a card is safe. The card
  code degrades to the previous behaviour. The rest of the firmware change
  (control loop keeps running through a connectivity loss) applies to every
  unit and should be validated on a WiFi unit before a wide rollout.
