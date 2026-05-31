#include "src/storage/page_cache.h"

#include "src/pure/hashing.h"   // prefKeyForBook

// ============================================================================
//  On-disk page-offset cache
//
//  File format (little-endian):
//    uint32 magic            kPageCacheMagic
//    uint32 layoutVersion    encodeLayoutVersion(layout)
//                            — bodySize/lineGap/family/bionic/statusbarReserve
//    uint32 fileSize         source-book size at save time
//    uint16 count            number of entries that follow
//    uint32 offsets[count]   byte offsets of pages 0..count-1
//
//  Magic history:
//    0x50434F46 — original, no stamp
//    0x50434F47 — added 16-bit `(bodySize, lineGap)` stamp
//    0x50434F48 — widened stamp to 32 bits to also cover font family + bionic
//    0x50434F49 — added statusbar-reserve byte to the 32-bit stamp; cache
//                 must rebuild when Statusbar::setMode toggles between
//                 Full / Minimal / Hidden because each has a different
//                 reserve height and therefore a different maxLines
//
//  Old files fail the magic check, get ignored, then overwritten on the next
//  save. No migration code needed.
// ============================================================================

static constexpr uint32_t kPageCacheMagic = 0x50434F49UL;

static constexpr size_t kHeaderBytes =
    sizeof(uint32_t)   // magic
  + sizeof(uint32_t)   // layoutVersion
  + sizeof(uint32_t)   // fileSize
  + sizeof(uint16_t);  // count

// Compact encoding of "what layout were the offsets in this file computed
// under?" — every field tucks into one byte (bodySize ∈ {8,10,12,14},
// lineGap ∈ [0,4], family ∈ {0,1}, bionic ∈ {0,1}, statusbarReserve in
// pixels — currently 0/1/STATUS_H). family + bionic share the third byte
// (4 bits each) to leave room for statusbarReserve in the top byte.
static uint32_t encodeLayoutVersion(const PageCacheLayout& layout) {
  return ((uint32_t)(layout.bodySize         & 0xFF))
       | ((uint32_t)(layout.lineGap          & 0xFF) << 8)
       | ((uint32_t)(layout.family           & 0x0F) << 16)
       | ((uint32_t)(layout.bionic           & 0x0F) << 20)
       | ((uint32_t)(layout.statusbarReserve & 0xFF) << 24);
}

static String pageCachePathForBook(const String& path) {
  return String("/pc_") + prefKeyForBook(path) + ".bin";
}

// Open the cache file for `path`, read the header (magic, layout stamp,
// stored file size, count), validate magic + layout + non-zero count, and
// return the file positioned just past the header along with the stored
// fileSize and count. On false, any opened file is closed and out-params
// are left untouched. `mode` is the LittleFS open mode — "r" for read-only
// loaders, "r+" for in-place append.
//
// fileSize is reported back rather than checked here; almost every caller
// then wants to enforce a size match against an expected value, which
// `openAndValidateCache` does in one step. The raw form exists only for
// `readCacheProgress`, which deliberately probes the header before the
// book file is open (and therefore can't yet know the actual book size).
static bool openCacheAndReadHeader(const String& path, const char* mode,
                                   const PageCacheLayout& layout,
                                   File& outFile,
                                   uint32_t& outStoredFileSize,
                                   uint16_t& outCount) {
  File f = FS.open(pageCachePathForBook(path), mode);
  if (!f) return false;

  uint32_t magic = 0;
  uint32_t layoutVersion = 0;
  uint32_t fileSize = 0;
  uint16_t count = 0;

  if (f.read((uint8_t*)&magic, sizeof(magic)) != sizeof(magic))                         { f.close(); return false; }
  if (f.read((uint8_t*)&layoutVersion, sizeof(layoutVersion)) != sizeof(layoutVersion)) { f.close(); return false; }
  if (f.read((uint8_t*)&fileSize, sizeof(fileSize)) != sizeof(fileSize))                { f.close(); return false; }
  if (f.read((uint8_t*)&count, sizeof(count)) != sizeof(count))                         { f.close(); return false; }

  if (magic != kPageCacheMagic
      || layoutVersion != encodeLayoutVersion(layout)
      || count == 0) {
    f.close();
    return false;
  }

  outFile           = f;
  outStoredFileSize = fileSize;
  outCount          = count;
  return true;
}

// Same as `openCacheAndReadHeader`, but also enforces that the cache's
// stored fileSize matches `expectedSize` and hides the value from the
// caller — so a function that asks for a validated cache cannot forget
// the fileSize check. Used by every read/write path that has the book
// (or expects one of a known size) open. Closes the file on mismatch.
static bool openAndValidateCache(const String& path, const char* mode,
                               size_t expectedSize,
                               const PageCacheLayout& layout,
                               File& outFile, uint16_t& outCount) {
  uint32_t storedFileSize = 0;
  if (!openCacheAndReadHeader(path, mode, layout, outFile, storedFileSize, outCount)) {
    return false;
  }
  if (storedFileSize != (uint32_t)expectedSize) {
    outFile.close();
    return false;
  }
  return true;
}

bool loadPageOffsetCacheForBook(const String& path, size_t expectedSize,
                                const PageCacheLayout& layout,
                                PageOffsetTable& out) {
  File f;
  uint16_t count = 0;
  if (!openAndValidateCache(path, "r", expectedSize, layout, f, count)) return false;
  if (count > MAX_PAGES) { f.close(); return false; }

  int loaded = 0;
  for (uint16_t i = 0; i < count; i++) {
    uint32_t off = 0;
    if (f.read((uint8_t*)&off, sizeof(off)) != sizeof(off)) break;
    out.offsets[i] = off;
    loaded++;
  }
  f.close();

  if (loaded == 0) return false;
  out.count = loaded;
  return true;
}

void writeFreshCache(const String& path, size_t fileSize,
                     const PageCacheLayout& layout,
                     const uint32_t* offsets, uint16_t n) {
  if (n == 0) return;

  File f = FS.open(pageCachePathForBook(path), "w");
  if (!f) return;

  uint32_t magic = kPageCacheMagic;
  uint32_t layoutVersion = encodeLayoutVersion(layout);
  uint32_t size32 = (uint32_t)fileSize;
  uint16_t count16 = (uint16_t)min((int)n, MAX_PAGES);

  f.write((const uint8_t*)&magic,         sizeof(magic));
  f.write((const uint8_t*)&layoutVersion, sizeof(layoutVersion));
  f.write((const uint8_t*)&size32,        sizeof(size32));
  f.write((const uint8_t*)&count16,       sizeof(count16));
  f.write((const uint8_t*)offsets,        count16 * sizeof(uint32_t));
  f.close();
}

void savePageOffsetCacheForBook(const String& path, size_t fileSize,
                                const PageCacheLayout& layout,
                                const PageOffsetTable& in) {
  // count <= 1 is a degenerate "just the seed entry" — nothing useful to
  // persist (rebuilds trivially on next open).
  if (in.count <= 1) return;
  writeFreshCache(path, fileSize, layout, in.offsets, (uint16_t)in.count);
}

bool appendPagesToCache(const String& path, size_t fileSize,
                        const PageCacheLayout& layout,
                        uint16_t currentCount,
                        const uint32_t* newOffsets, uint16_t n) {
  if (n == 0) return true;
  if ((size_t)currentCount + (size_t)n > MAX_PAGES) return false;

  File f;
  uint16_t storedCount = 0;
  if (!openAndValidateCache(path, "r+", fileSize, layout, f, storedCount)) return false;
  if (storedCount != currentCount) { f.close(); return false; }

  // Append the new offsets at the end of the file.
  size_t appendPos = kHeaderBytes + (size_t)currentCount * sizeof(uint32_t);
  if (!f.seek(appendPos))                                                       { f.close(); return false; }
  if (f.write((const uint8_t*)newOffsets, n * sizeof(uint32_t)) != n * sizeof(uint32_t)) {
    f.close();
    return false;
  }

  // Update count LAST so any concurrent reader (or a power-loss replay)
  // either sees the pre-append state (old count, old offsets) or the
  // fully-appended state (new count, new offsets) — never something in
  // between. LittleFS's flush is what makes that atomic for other handles.
  uint16_t newCount = (uint16_t)(currentCount + n);
  size_t countPos = sizeof(uint32_t) * 3;  // past magic, layoutVersion, fileSize
  if (!f.seek(countPos))                                                          { f.close(); return false; }
  if (f.write((const uint8_t*)&newCount, sizeof(newCount)) != sizeof(newCount))   { f.close(); return false; }
  f.flush();
  f.close();
  return true;
}

int loadOffsetForPageFromDisk(const String& path, size_t expectedSize,
                              const PageCacheLayout& layout,
                              int maxPage, uint32_t* out) {
  if (maxPage < 0) return -1;

  File f;
  uint16_t count = 0;
  if (!openAndValidateCache(path, "r", expectedSize, layout, f, count)) return -1;

  int targetPage = (maxPage >= (int)count) ? (int)count - 1 : maxPage;
  size_t entryPos = kHeaderBytes + (size_t)targetPage * sizeof(uint32_t);
  if (!f.seek(entryPos)) { f.close(); return -1; }

  uint32_t off = 0;
  if (f.read((uint8_t*)&off, sizeof(off)) != sizeof(off)) { f.close(); return -1; }
  f.close();

  *out = off;
  return targetPage;
}

bool readCacheProgress(const String& path, const PageCacheLayout& layout,
                       uint16_t* outCount, uint32_t* outLastOffset,
                       uint32_t* outStoredFileSize) {
  File f;
  uint32_t storedFileSize = 0;
  uint16_t count = 0;
  if (!openCacheAndReadHeader(path, "r", layout, f, storedFileSize, count)) return false;

  // Seek to the last offset entry and read it.
  size_t entryPos = kHeaderBytes + (size_t)(count - 1) * sizeof(uint32_t);
  if (!f.seek(entryPos)) { f.close(); return false; }
  uint32_t off = 0;
  if (f.read((uint8_t*)&off, sizeof(off)) != sizeof(off)) { f.close(); return false; }
  f.close();

  *outCount          = count;
  *outLastOffset     = off;
  *outStoredFileSize = storedFileSize;
  return true;
}

void deletePageCacheForBook(const String& path) {
  String cachePath = pageCachePathForBook(path);
  if (FS.exists(cachePath)) FS.remove(cachePath);
}

void renamePageCacheForBook(const String& oldPath, const String& newPath) {
  String oldCache = pageCachePathForBook(oldPath);
  if (!FS.exists(oldCache)) return;
  String newCache = pageCachePathForBook(newPath);
  if (FS.exists(newCache)) FS.remove(newCache);
  FS.rename(oldCache, newCache);
}
