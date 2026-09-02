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
