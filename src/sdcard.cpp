// =====================================================
// SDCARD.CPP — microSD on SPI2 (SdFat / FAT32)
// =====================================================

#include "sdcard.h"
#include "logger.h"
#include <SPI.h>
#include <SdFat.h>

// SPI2 on the ESP32-S3 is the "FSPI" peripheral. Pins are remapped explicitly
// below via the GPIO matrix, so the peripheral choice only needs to be a free
// controller — FSPI matches the board's "SPI2" labelling.
static SdFat32   sd;
static SPIClass  sdSpi(FSPI);
static bool      mounted = false;

bool sdInit()
{
  if (mounted) return true;

  pinMode(SD_CD_PIN, INPUT_PULLUP);
  sdSpi.begin(SD_SPI_SCK, SD_SPI_MISO, SD_SPI_MOSI, SD_CS_PIN);

  SdSpiConfig cfg(SD_CS_PIN, SHARED_SPI, SD_SPI_HZ, &sdSpi);
  mounted = sd.begin(cfg);

  if (mounted)
  {
    sd.mkdir("/config");
    sd.mkdir("/state");
    sd.mkdir("/buffer");
    LOGF("[SD] mounted, free %llu MB\n", sdFreeBytes() / (1024ULL * 1024ULL));
  }
  else
  {
    LOGLN("[SD] mount failed / no card");
  }
  return mounted;
}

bool sdMounted()    { return mounted; }
bool sdCardDetect() { return digitalRead(SD_CD_PIN) == LOW; }

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
