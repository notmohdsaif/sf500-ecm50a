#pragma once
#include <Arduino.h>

// =====================================================
// SDCARD.H — microSD on the ECM50-A's SPI2 bus
// Power-loss-safe primitives for offline buffering + config persistence.
// SPI2 is a dedicated bus (no contention with Ethernet SPI1 or the modem UART2).
// Pin map: ECM50-A_Pin_List_V1.0.xlsx, "TF_Card".
// =====================================================

#define SD_SPI_SCK   10
#define SD_SPI_MISO  9
#define SD_SPI_MOSI  46
#define SD_CS_PIN    1
#define SD_CD_PIN    3
#define SD_SPI_HZ    (20UL * 1000UL * 1000UL)

// Mount the card. Idempotent — safe to call again; returns the current state.
// Creates /config, /state and /buffer on first successful mount.
bool     sdInit();
bool     sdMounted();

// Coarse SD health for telemetry (boot summary + MQTT payload). UNREADABLE vs
// ABSENT is inferred from the CD pin, so on a board whose CD line is not wired
// a truly card-less slot may report UNREADABLE — treat "not OK" as the signal.
enum SdHealth { SD_HEALTH_OK, SD_HEALTH_ABSENT, SD_HEALTH_UNREADABLE };
SdHealth sdHealth();

// Free space, recomputed at most every 5 min (the underlying FAT scan is not
// free). 0 when no card. Safe to call every telemetry publish.
uint64_t sdFreeBytesCached();

// Total card capacity, MB. 0 when no card. Cheap: sectorCount() is a value
// SdFat cached from the card's CSD register at mount, no SD I/O per call.
uint32_t sdTotalMbCached();

enum SdEvent { SD_EVT_NONE, SD_EVT_REMOVED, SD_EVT_REMOUNTED };

// Poll from loop(): debounced card-detect handling. Drops the mount on a pull
// (writes then no-op) and remounts on reinsert without a reboot. Returns the
// transition that just occurred, if any, so the caller can log it.
SdEvent  sdTick();

// Recovery: wipe the card and lay down a fresh FAT32 filesystem, then remount.
// Destroys all data on the card. Only ever invoked by the `SDFORMAT CONFIRM`
// serial command — never called automatically. Use when a physically-present
// card will not mount (exFAT from the factory, unformatted, or corrupted FS).
bool     sdFormatFat32();

// Raw card-detect line (GPIO3). Polarity confirmed on the bench (Task 0.4):
// LOW = card present with the INPUT_PULLUP used here.
bool     sdCardDetect();

uint64_t sdFreeBytes();

// Write `path` via a temp file + fsync + rename, so a power cut never leaves a
// half-written file at `path` (it keeps the previous contents, or nothing).
bool     sdAtomicWrite(const char* path, const uint8_t* data, size_t len);

// Append one line (`line` + '\n') with a single O_APPEND write + fsync.
bool     sdAppendLine(const char* path, const String& line);

bool     sdReadFile(const char* path, String& out);
size_t   sdFileSize(const char* path);

// Read at most `maxLen` bytes of `path` starting at byte `offset` into `out`.
// For streaming large journals without slurping the whole file into RAM.
bool     sdReadRange(const char* path, size_t offset, size_t maxLen, String& out);

// Rewrite `path` keeping only bytes [dropBytes, end), streamed through a temp
// file + rename (power-loss safe, bounded RAM). Used for journal compaction.
bool     sdStreamDropPrefix(const char* path, size_t dropBytes);

// Reconcile a crash-safe file swap (sdStreamDropPrefix / sdRewriteCommit) that
// a power cut interrupted mid-rename. Call once at boot after the card mounts.
void     sdFinishInterruptedSwap(const char* path);

// Stateful streamed rewrite of a file: sdRewriteBegin(path) opens <path>.tmp,
// sdRewriteAppend() writes into it, sdRewriteCommit() fsyncs + renames it over
// <path>, sdRewriteAbort() discards it. One open/close for the whole rewrite,
// bounded RAM. Only one rewrite may be in flight at a time.
bool     sdRewriteBegin(const char* finalPath);
bool     sdRewriteAppend(const uint8_t* data, size_t len);
bool     sdRewriteCommit();
void     sdRewriteAbort();
