// =====================================================
// SDCARD.CPP — microSD on SPI2 (SdFat / FAT32)
// =====================================================

#include "sdcard.h"
#include "logger.h"
#include <SPI.h>
#include <SdFat.h>
#include <esp_task_wdt.h>

// SPI2 on the ESP32-S3 is the "FSPI" peripheral. Pins are remapped explicitly
// below via the GPIO matrix, so the peripheral choice only needs to be a free
// controller — FSPI matches the board's "SPI2" labelling.
//
// Bench-confirmed on sf500_107888 (Task 0.4, 2026-09-03): pin map below is
// correct, the card mounts first-try at 20 MHz SHARED_SPI, and CD (GPIO3)
// reads LOW when a card is seated.
static SdFat32   sd;
static SPIClass  sdSpi(FSPI);
static bool      mounted   = false;
static bool      busInited = false;

static uint64_t  freeCache   = 0;
static uint32_t  freeCacheAt = 0;   // millis() of last freeCache refresh; 0 = stale

#define SD_REMOUNT_RETRY_MS 15000UL   // don't hammer sd.begin() when a card won't mount

bool sdInit()
{
  if (mounted) return true;

  if (!busInited)
  {
    pinMode(SD_CD_PIN, INPUT_PULLUP);
    sdSpi.begin(SD_SPI_SCK, SD_SPI_MISO, SD_SPI_MOSI, SD_CS_PIN);
    busInited = true;
  }

  SdSpiConfig cfg(SD_CS_PIN, SHARED_SPI, SD_SPI_HZ, &sdSpi);
  mounted = sd.begin(cfg);

  if (mounted)
  {
    sd.mkdir("/config");
    sd.mkdir("/state");
    sd.mkdir("/buffer");
    freeCacheAt = 0;                            // force a fresh free-space read
    LOGF("[SD] mounted, free %llu MB\n", sdFreeBytesCached() / (1024ULL * 1024ULL));
  }
  else
  {
    // A card that is physically present but unreadable here is almost always
    // exFAT/unformatted (SdFat32 mounts FAT16/FAT32 only). `SDFORMAT CONFIRM`
    // on the serial console rewrites it as FAT32 in place.
    LOGLN("[SD] mount failed / no card — if a card is inserted, try SDFORMAT");
  }
  return mounted;
}

SdEvent sdTick()
{
  // Card-detect edge tracking with a short debounce. A pull mid-run drops the
  // mounted flag so every buffer write cleanly no-ops (callers fall back to a
  // direct POST); a reinsert remounts without a reboot.
  static bool     lastRaw     = false;
  static uint32_t stableSince = 0;
  static bool     primed      = false;

  bool     raw = sdCardDetect();
  uint32_t now = millis();

  if (!primed) { lastRaw = raw; stableSince = now; primed = true; return SD_EVT_NONE; }
  if (raw != lastRaw) { lastRaw = raw; stableSince = now; return SD_EVT_NONE; }
  if (now - stableSince < 400) return SD_EVT_NONE;   // wait for the level to settle

  if (mounted && !raw)
  {
    LOGLN("[SD] card removed — buffering + schedule persistence paused");
    mounted = false;                            // stale `sd` object is unused while false
    freeCacheAt = 0;
    return SD_EVT_REMOVED;
  }
  if (!mounted && raw)
  {
    // A card is (or looks) present but isn't mounted. Retry sd.begin() at most
    // once per SD_REMOUNT_RETRY_MS so a bad/absent card (or an unwired CD line
    // reading "present") never spins the SPI init every loop.
    static uint32_t lastTry = 0;
    if (lastTry == 0 || now - lastTry >= SD_REMOUNT_RETRY_MS)
    {
      lastTry = now;
      if (sdInit())
      {
        LOGLN("[SD] card reinserted — remounted");
        return SD_EVT_REMOUNTED;
      }
    }
  }
  return SD_EVT_NONE;
}

bool sdMounted()    { return mounted; }
bool sdCardDetect() { return digitalRead(SD_CD_PIN) == LOW; }

SdHealth sdHealth()
{
  if (mounted)         return SD_HEALTH_OK;
  if (sdCardDetect())  return SD_HEALTH_UNREADABLE;   // CD says present, won't mount
  return SD_HEALTH_ABSENT;
}

uint64_t sdFreeBytesCached()
{
  if (!mounted) return 0;
  uint32_t now = millis();
  if (freeCacheAt == 0 || now - freeCacheAt >= 300000UL)   // refresh at most every 5 min
  {
    freeCache   = sdFreeBytes();
    freeCacheAt = now ? now : 1;
  }
  return freeCache;
}

bool sdFormatFat32()
{
  // Reuse the card object sdInit()'s probe already brought up. Re-running
  // sdSpi.begin() / cardBegin() here wedges the SPI transaction (loop task
  // blocks, both cores go idle). Only re-init if the probe never ran.
  if (!sd.card() || sd.card()->sectorCount() == 0)
  {
    SdSpiConfig cfg(SD_CS_PIN, SHARED_SPI, SD_SPI_HZ, &sdSpi);
    if (!sd.cardBegin(cfg))
    {
      LOGF("[SD.format] no usable card — sdErrorCode 0x%02X\n", sd.sdErrorCode());
      return false;
    }
  }

  uint32_t sectors = sd.card()->sectorCount();
  LOGF("[SD.format] wiping %lu sectors (~%lu MB) and writing FAT32 — may take "
       "several minutes...\n",
       (unsigned long)sectors, (unsigned long)(sectors / 2048UL));
  Serial.flush();

  // format() blocks the loop task for minutes and starves the idle tasks the
  // task-WDT also watches. init() refuses to reconfigure a live WDT, so fully
  // remove this task, deinit the WDT, then restore it afterwards.
  esp_task_wdt_delete(NULL);
  esp_task_wdt_deinit();
  bool fmtOk = sd.format(&Serial);
  esp_task_wdt_init(60, true);
  esp_task_wdt_add(NULL);
  esp_task_wdt_reset();

  if (!fmtOk)
  {
    LOGF("\n[SD.format] FAILED — sdErrorCode 0x%02X\n", sd.sdErrorCode());
    mounted = false;
    return false;
  }

  mounted = sd.volumeBegin();
  if (mounted)
  {
    sd.mkdir("/config");
    sd.mkdir("/state");
    sd.mkdir("/buffer");
    LOGF("\n[SD.format] done — FAT%d, free %llu MB\n",
         sd.vol()->fatType(), sdFreeBytes() / (1024ULL * 1024ULL));
  }
  else
  {
    LOGF("\n[SD.format] volumeBegin after format failed — sdErrorCode 0x%02X\n",
         sd.sdErrorCode());
  }
  return mounted;
}

uint64_t sdFreeBytes()
{
  if (!mounted) return 0;
  return (uint64_t)sd.vol()->freeClusterCount() * sd.vol()->bytesPerCluster();
}

bool sdAtomicWrite(const char* path, const uint8_t* data, size_t len)
{
  if (!mounted) return false;

  String tmp = String(path) + ".tmp";

  File32 f = sd.open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC);
  if (!f) return false;

  bool ok = (f.write(data, len) == len);
  f.sync();
  f.close();

  if (!ok)
  {
    sd.remove(tmp.c_str());
    return false;
  }

  sd.remove(path);                       // rename() will not overwrite
  return sd.rename(tmp.c_str(), path);
}

bool sdAppendLine(const char* path, const String& line)
{
  if (!mounted) return false;

  File32 f = sd.open(path, O_WRONLY | O_CREAT | O_APPEND);
  if (!f) return false;

  size_t want = line.length() + 1;
  size_t n = f.write((const uint8_t*)line.c_str(), line.length());
  n += f.write((const uint8_t*)"\n", 1);
  f.sync();
  f.close();

  return n == want;
}

bool sdReadFile(const char* path, String& out)
{
  if (!mounted) return false;

  File32 f = sd.open(path, O_RDONLY);
  if (!f) return false;

  out = "";
  out.reserve(f.fileSize());
  while (f.available())
    out += (char)f.read();
  f.close();
  return true;
}

size_t sdFileSize(const char* path)
{
  if (!mounted) return 0;

  File32 f = sd.open(path, O_RDONLY);
  if (!f) return 0;
  size_t s = f.fileSize();
  f.close();
  return s;
}

bool sdReadRange(const char* path, size_t offset, size_t maxLen, String& out)
{
  out = "";
  if (!mounted) return false;

  File32 f = sd.open(path, O_RDONLY);
  if (!f) return false;
  if (!f.seekSet(offset)) { f.close(); return false; }

  out.reserve(maxLen < 4096 ? maxLen : 4096);
  uint8_t buf[512];
  size_t got = 0;
  while (got < maxLen)
  {
    size_t want = maxLen - got;
    if (want > sizeof(buf)) want = sizeof(buf);
    int n = f.read(buf, want);
    if (n <= 0) break;
    for (int i = 0; i < n; i++) out += (char)buf[i];
    got += n;
  }
  f.close();
  return true;
}

bool sdStreamDropPrefix(const char* path, size_t dropBytes)
{
  if (!mounted) return false;

  File32 in = sd.open(path, O_RDONLY);
  if (!in) return false;
  size_t total = in.fileSize();
  if (dropBytes >= total) { in.close(); sd.remove(path); return true; }
  if (!in.seekSet(dropBytes)) { in.close(); return false; }

  String tmp = String(path) + ".tmp";
  File32 out = sd.open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC);
  if (!out) { in.close(); return false; }

  uint8_t buf[512];
  bool ok = true;
  int n;
  while ((n = in.read(buf, sizeof(buf))) > 0)
    if (out.write(buf, n) != (size_t)n) { ok = false; break; }

  out.sync();
  out.close();
  in.close();
  if (!ok) { sd.remove(tmp.c_str()); return false; }

  sd.remove(path);
  return sd.rename(tmp.c_str(), path);
}

// --- Stateful streamed rewrite: one open, many appends, one atomic commit. ---
// For rewriting a large file (e.g. journal retention) without holding it in RAM
// and without an open/close per line.
static File32 rwOut;
static String rwFinal;
static bool   rwOpen = false;

bool sdRewriteBegin(const char* finalPath)
{
  if (!mounted || rwOpen) return false;
  rwFinal = String(finalPath);
  String tmp = rwFinal + ".tmp";
  rwOut = sd.open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC);
  if (!rwOut) return false;
  rwOpen = true;
  return true;
}

bool sdRewriteAppend(const uint8_t* data, size_t len)
{
  if (!rwOpen) return false;
  return rwOut.write(data, len) == len;
}

bool sdRewriteCommit()
{
  if (!rwOpen) return false;
  rwOut.sync();
  rwOut.close();
  rwOpen = false;
  String tmp = rwFinal + ".tmp";
  sd.remove(rwFinal.c_str());                 // rename() will not overwrite
  return sd.rename(tmp.c_str(), rwFinal.c_str());
}

void sdRewriteAbort()
{
  if (!rwOpen) return;
  rwOut.close();
  rwOpen = false;
  String tmp = rwFinal + ".tmp";
  sd.remove(tmp.c_str());
}
