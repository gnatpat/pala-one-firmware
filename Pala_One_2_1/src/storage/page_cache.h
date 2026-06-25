#ifndef PALA_STORAGE_PAGE_CACHE_H
#define PALA_STORAGE_PAGE_CACHE_H

#include "src/config.h"
#include "src/state.h"
#include "src/pure/page_offset_table.h"

// ============================================================================
//  On-disk page-offset cache (pc_<hash>.bin files in LittleFS root)
//
//  Per-book binary file mapping page index -> byte offset, stamped with the
//  PageCacheLayout under which the offsets were computed.
//  Load rejects mismatched files; stale entries are silently overwritten on
//  the next save. No external "invalidate everything" pass needed on font or
//  layout change — layout-correctness is a property of the file format itself.
//
//  Layout values come from `Font::layoutForCache()` (see font.h). Anything
//  that changes how the body paginates (body size, line gap, font family,
//  bionic, statusbar reserve height) flows into that one struct, so this
//  module never has to know what specifically affects layout.
//
//  The active reader bulk-loads the whole table into `g_bookview.pages` via
//  `loadPageOffsetCacheForBook`. Cross-book lookups (web bookmark resolve,
//  page-text export) use the lighter `loadOffsetForPageFromDisk` to read
//  one entry without allocating a 40 KB scratch table.
// ============================================================================

// Anything that affects page layout — and therefore must invalidate the
// cache when changed. Body face is identified by the same (size, family,
// bionic) tuple the Font module exposes; line gap is the user's spacing
// preference; statusbar reserve is how many pixels the bottom statusbar
// steals from the text area (which changes the page's `maxLines`).
// Packed into the on-disk header at save time; the load path rejects files
// whose stamp doesn't match the *current* layout.
//
// New fields go at the end and bump the on-disk magic in page_cache.cpp so
// older caches are silently rejected and re-built on the next save.
struct PageCacheLayout {
  int     bodySize;          // 8/10/12/14
  int     lineGap;           // [0, 4]
  uint8_t family;            // matches Font::Family numeric value (0 = Helv, 1 = Dys)
  uint8_t bionic;            // 0 / 1
  uint8_t statusbarReserve;  // pixels reserved at the bottom; from Statusbar::reserveH()

  // Equality lives right next to the fields so adding a new layout
  // dimension naturally surfaces the comparison that needs updating —
  // used by the idle paginator's "are inputs still the same?" check.
  bool operator==(const PageCacheLayout& o) const {
    return bodySize         == o.bodySize
        && lineGap          == o.lineGap
        && family           == o.family
        && bionic           == o.bionic
        && statusbarReserve == o.statusbarReserve;
  }
};

// Bulk-load the persisted offset table for `path` into `out`. Layout-stamped
// at save time; load rejects any file whose stamp doesn't match `layout`.
// Returns true on success; on false, `out` is left untouched (callers
// typically seed `offsets[0]=0, count=1` themselves).
bool loadPageOffsetCacheForBook(const String& path, size_t expectedSize,
                                const PageCacheLayout& layout,
                                PageOffsetTable& out);

// Persist `in` for `path`, stamped with `layout`. No-op if `in.count <= 1`
// (nothing useful to save).
void savePageOffsetCacheForBook(const String& path, size_t fileSize,
                                const PageCacheLayout& layout,
                                const PageOffsetTable& in);

// Write a fresh cache file for `path` from a raw offsets array (truncates
// any existing file). Equivalent to `savePageOffsetCacheForBook` but takes
// a raw pointer rather than the 40 KB `PageOffsetTable` struct — useful for
// callers (the idle paginator) that work in small batches and can't afford
// the full-table allocation. No-op if `n == 0`.
void writeFreshCache(const String& path, size_t fileSize,
                     const PageCacheLayout& layout,
                     const uint32_t* offsets, uint16_t n);

// Append `n` already-computed offsets to an existing cache file. Validates
// the header matches the current (magic, layout, fileSize); on any mismatch
// or absence, returns false without touching the file — caller seeds a
// fresh cache via `savePageOffsetCacheForBook` instead.
//
// On success the file ends with the new entries appended and the header's
// `count` bumped by `n`. The count update happens LAST within the batch
// and the whole batch is committed by a single `flush()` — under LittleFS's
// COW + journal semantics, any other handle either sees the pre-batch state
// or the fully-appended state. There is no torn-state window for readers,
// and power loss mid-batch rolls back to the pre-batch state.
//
// `currentCount` is what the caller believes is in the header right now;
// the function reads it back to defend against drift. Returns false if the
// readback disagrees (something else mutated the file behind our back).
bool appendPagesToCache(const String& path, size_t fileSize,
                        const PageCacheLayout& layout,
                        uint16_t currentCount,
                        const uint32_t* newOffsets, uint16_t n);

// Single-entry on-disk lookup: read header, validate magic + layout +
// expected file size, and return the offset of the largest cached page
// `<= maxPage` along with that page's index. Constant-RAM (no PageOffsetTable
// scratch); two short reads (header + one offset). Returns -1 (and leaves
// `*out` untouched) if the cache file is absent, stamped for a different
// layout, sized for a different file, or has zero entries.
int loadOffsetForPageFromDisk(const String& path, size_t expectedSize,
                              const PageCacheLayout& layout,
                              int maxPage, uint32_t* out);

// Does the on-disk cache for `path` already cover byte `targetOffset` under
// `layout`? Used at book-open time to decide whether opening will be fast
// (cache covers the read position → instant) or slow (needs pagination
// first → loading screen). Returns false on absent / layout-mismatched
// cache, since those also require pagination from scratch.
//
// Skips the fileSize check on purpose: callers asking this question don't
// yet have the book open (avoiding the open is half the point). A stale-
// fileSize cache that happens to be far enough will be re-validated on
// the actual book open path; the worst outcome is a missed loading screen
// for a book that was just replaced, which is no worse than today.
bool pageCacheCoversOffset(const String& path, const PageCacheLayout& layout,
                           uint32_t targetOffset);

// Lightweight "how far have we cached?" probe used by the background
// paginator. Returns true if the cache file exists with matching magic +
// layout + non-zero count; `*outCount` and `*outLastOffset` carry the
// count and the offset at the highest cached page, and `*outStoredFileSize`
// returns the book-file size recorded in the cache header at save time
// (for the caller to compare against the current book size — done in a
// separate step so the paginator can decide "no more work needed" without
// yet having opened the book to know its size). Returns false on absence
// or layout mismatch.
bool readCacheProgress(const String& path, const PageCacheLayout& layout,
                       uint16_t* outCount, uint32_t* outLastOffset,
                       uint32_t* outStoredFileSize);

// Remove the on-disk page-cache file for `path` (no-op if absent).
void deletePageCacheForBook(const String& path);

// Move the on-disk page-cache file from `oldPath` to `newPath` (no-op if
// no source file). If a stale destination exists, it's removed first.
void renamePageCacheForBook(const String& oldPath, const String& newPath);

#endif  // PALA_STORAGE_PAGE_CACHE_H
