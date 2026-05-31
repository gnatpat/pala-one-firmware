#include "src/ui/idle_paginator.h"

#include "src/config.h"                          // MAX_PAGES
#include "src/hal/input.h"                       // buttonQueueNonEmpty
#include "src/pure/hashing.h"                    // prefKeyForBook
#include "src/state.h"                           // prefs, FS
#include "src/storage/book_metadata.h"           // loadSavedOffset / loadSavedPage
#include "src/storage/page_cache.h"
#include "src/storage/preferences_store.h"       // PreferencesStore
#include "src/ui/font.h"                         // layoutForCache
#include "src/ui/reader.h"                       // g_bookview (reader-active gate)
#include "src/ui/text.h"                         // nextPageOffset

// Diagnostic logging — only fires on state transitions and batch flushes,
// never on the cheap-path no-ops, so it's safe to leave on without spam.
// Grep "IdlePaginator" on serial to follow what the paginator's doing.
#define IPLOG(fmt, ...) Serial.printf("[IdlePaginator] " fmt "\n", ##__VA_ARGS__)

namespace IdlePaginator {

static constexpr const char* kPrefKey = "pg_recent";

// In-memory copy of the most-recently-saved book path. Avoids an NVS
// write when the same book is re-opened back-to-back.
static String s_lastSaved = "";

// RAM-only record of "the pagination goal we have already achieved."
// `tick()` short-circuits when the current inputs match this — no need
// to open the cache file to re-verify. Default values (empty path, zero
// layout) intentionally can never match a real, validated tick: the path
// gate above the memo check guarantees a non-empty current path, so no
// `valid` flag is needed.
//
// Wiped on deep sleep / reboot — fine, the next tick just redoes one
// real check and re-arms it.
struct PaginationGoal {
  String          path;
  PageCacheLayout layout       = {};
  uint32_t        targetOffset = 0;
  int             targetPage   = -1;

  bool matches(const String& p, const PageCacheLayout& l,
               uint32_t off, int pg) const {
    return path         == p
        && layout       == l
        && targetOffset == off
        && targetPage   == pg;
  }
};
static PaginationGoal s_completedGoal;

// Max pages computed per tick — caps the stack buffer for pending offsets.
// Generously sized so it's effectively never hit at our current budget;
// if a future budget bump pushes through it, the loop breaks early, the
// single end-of-tick flush captures what we have, and the next tick
// continues. At 4 bytes/entry this costs 256 bytes of stack.
static constexpr int kMaxPagesPerTick = 64;

// Have we paginated far enough to cover where the user has read? Offset is
// the canonical layout-invariant target; targetPage is a legacy fallback
// for pre-offset firmware. Used by both the early caught-up gate (before
// opening the book) and the work-loop break condition.
static bool paginatedFarEnough(uint32_t lastOffset, int count,
                               uint32_t targetOffset, int targetPage) {
  if (targetOffset != kOffsetUnset) return lastOffset >= targetOffset;
  return count > targetPage;
}

// Record "we have caught up under these inputs" so subsequent ticks can
// short-circuit before opening the cache file. Called from every code
// path inside `tick()` that returns false because no further work is
// needed (early caught-up gate + natural completion of the work loop).
// Crucially NOT called from interrupted paths (button / budget) — those
// have work pending and lying here would silently skip future ticks.
static void recordCompletedGoal(const String& path, const PageCacheLayout& layout,
                                uint32_t targetOffset, int targetPage) {
  s_completedGoal.path         = path;
  s_completedGoal.layout       = layout;
  s_completedGoal.targetOffset = targetOffset;
  s_completedGoal.targetPage   = targetPage;
}

void saveLastOpenedBook(const String& path) {
  if (path.length() == 0) return;
  if (path == s_lastSaved) return;
  prefs.putString(kPrefKey, path);
  s_lastSaved = path;
  IPLOG("tracking: %s", path.c_str());
}

bool tick(uint32_t budgetMs) {
  // Cheap-path gates first — these are the common case (no work to do)
  // and must stay sub-millisecond so we can call tick() unconditionally
  // from the main loop. `isKey` guards the getString so we don't spam
  // `nvs_get_str ... NOT_FOUND` errors at ERROR level on every tick when
  // no book has ever been opened.
  if (!prefs.isKey(kPrefKey)) return false;
  String path = prefs.getString(kPrefKey, "");
  if (path.length() == 0) return false;
  if (g_bookview.book.isOpen()) return false;

  uint32_t startMs = millis();

  // Decide how far to paginate. Only as far as the user has read: the
  // reader paginates the rest lazily as the user advances, so pre-doing
  // it would be wasted work. The byte offset is layout-invariant (saved
  // by the reader on every progress save), so it's a precise stop target
  // across font/family/lgap changes.
  PreferencesStore kv(prefs);
  String bookKey = prefKeyForBook(path);
  uint32_t targetOffset = loadSavedOffset(kv, bookKey);
  int targetPage = -1;
  if (targetOffset == kOffsetUnset) {
    // Legacy fallback: pre-offset firmware only saved a page number. Walk
    // until the page count covers it under the current layout.
    targetPage = loadSavedPage(kv, bookKey);
    if (targetPage <= 0) return false;  // never read past page 0; nothing to do
  }

  PageCacheLayout layout = Font::layoutForCache();

  // If nothing relevant has changed since the last tick that determined
  // we were caught up, skip the cache-file probe entirely. Layout changes,
  // web-UI jumps that update savedOffset, and any book open via the reader
  // all break one of these equalities naturally.
  if (s_completedGoal.matches(path, layout, targetOffset, targetPage)) {
    return false;
  }

  // Early caught-up check — probe the cache header BEFORE opening the
  // (potentially deeply-nested) book file. If the cache already covers
  // the saved offset for the current layout, opening will be instant and
  // we have nothing to do.
  uint16_t cachedCount = 0;
  uint32_t cachedLastOffset = 0;
  uint32_t cachedFileSize = 0;
  bool layoutMatch = readCacheProgress(path, layout,
                                       &cachedCount, &cachedLastOffset, &cachedFileSize);
  if (layoutMatch) {
    if (paginatedFarEnough(cachedLastOffset, cachedCount, targetOffset, targetPage)
        || cachedCount >= MAX_PAGES) {
      IPLOG("already caught up: book=%s pages=%u lastOffset=%u target=%u",
            path.c_str(), cachedCount, cachedLastOffset, targetOffset);
      recordCompletedGoal(path, layout, targetOffset, targetPage);
      return false;
    }
  }

  // Work to do — bump CPU to 240 MHz for the rest of this tick. Pagination
  // is CPU-bound text measurement (~3× slower at the menu's idle 80 MHz)
  // and the bump uses ~2× current for ~3× less time, so net energy is
  // actually lower. Restored at every exit below so we don't leak the
  // elevated frequency back to whatever screen scheduled us.
  uint32_t prevCpuMhz = getCpuFrequencyMhz();
  if (prevCpuMhz < 240) setCpuFrequencyMhz(240);

  File bookFile = FS.open(path, "r");
  if (!bookFile) {
    // Book is gone (deleted, renamed) — clear the stale key so we don't
    // keep retrying every loop iteration.
    IPLOG("book gone, clearing tracking: %s", path.c_str());
    prefs.remove(kPrefKey);
    s_lastSaved = "";
    if (prevCpuMhz < 240) setCpuFrequencyMhz(prevCpuMhz);
    return false;
  }
  size_t fileSize = bookFile.size();

  // The cache's fileSize check finally happens here, now that we know
  // the actual book size. Mismatch (or absent / layout-mismatched cache)
  // means we throw away whatever's there and start fresh from offset 0.
  bool freshStart = !layoutMatch || (cachedFileSize != (uint32_t)fileSize);
  int count = freshStart ? 0 : cachedCount;
  int startCount = count;  // for per-tick stats in the completion log
  uint32_t lastOffset = freshStart ? 0 : cachedLastOffset;
  IPLOG("starting work: book=%s freshStart=%d count=%d lastOffset=%u target=%u fileSize=%u",
        path.c_str(), (int)freshStart, count, lastOffset,
        targetOffset, (uint32_t)fileSize);

  // Mark "setup" boundary: everything up to here (prefs reads, cache
  // probe, book open) is fixed per-tick overhead; everything below is
  // actual pagination + writeback.
  uint32_t setupMs = (uint32_t)(millis() - startMs);

  // Work loop. Pages accumulate in `pending[]` and are flushed in a
  // single write at the end of the tick (or when the buffer fills, which
  // is a rare safety break — at our budget we never come close to it).
  // Single end-of-tick flush keeps per-tick I/O bounded to ONE cache
  // file open regardless of how many pages we computed; the cost of a
  // mid-tick crash is at most ~budget-ms of recomputation next tick.
  uint32_t pending[kMaxPagesPerTick];
  int pendingN = 0;
  bool didAnyWork = false;

  // Track whether the loop ends because we're caught up (any natural
  // break) vs. interrupted (button, budget, or buffer full). Only the
  // former is safe to memo — an interrupted tick still has work pending.
  bool completed = false;
  while (true) {
    if (buttonQueueNonEmpty()) break;
    if ((uint32_t)(millis() - startMs) >= budgetMs) break;
    if (pendingN >= kMaxPagesPerTick) break;  // buffer full; resume next tick
    if (count + pendingN >= MAX_PAGES)                                        { completed = true; break; }
    if (lastOffset >= (uint32_t)fileSize)                                     { completed = true; break; }
    // Stop once we've covered as far as the user has read — the reader
    // paginates the rest lazily as they advance, no need to pre-do it.
    if (paginatedFarEnough(lastOffset, count + pendingN, targetOffset, targetPage)) {
      completed = true; break;
    }

    uint32_t next = nextPageOffset(bookFile, lastOffset);
    if (next <= lastOffset) {
      // Stuck (zero-byte page) — treat as EOF so we don't spin.
      completed = true;
      break;
    }
    pending[pendingN++] = next;
    lastOffset = next;
    didAnyWork = true;
  }

  // Single end-of-tick flush.
  uint32_t flushMs = 0;
  if (pendingN > 0) {
    uint32_t t0 = millis();
    if (freshStart) {
      // First write for this layout: prepend seed offset 0 so page 0 is
      // in the cache. writeFreshCache truncates any stale file.
      uint32_t withSeed[kMaxPagesPerTick + 1];
      withSeed[0] = 0;
      memcpy(&withSeed[1], pending, pendingN * sizeof(uint32_t));
      writeFreshCache(path, fileSize, layout, withSeed, (uint16_t)(pendingN + 1));
      count = pendingN + 1;
    } else if (appendPagesToCache(path, fileSize, layout,
                                  (uint16_t)count, pending, (uint16_t)pendingN)) {
      count += pendingN;
    }
    flushMs = (uint32_t)(millis() - t0);
  }

  bookFile.close();
  uint32_t elapsedMs     = (uint32_t)(millis() - startMs);
  int      addedThisTick = count - startCount;
  // Decompose the elapsed time into the three meaningful phases:
  //   setup  — prefs reads + cache probe + book open (fixed per tick)
  //   paging — pure nextPageOffset (per-page CPU work — heavy at low MHz)
  //   flush  — cache file writes (one open+write+close per batch)
  uint32_t pagingMs = elapsedMs > (setupMs + flushMs)
                    ? elapsedMs - setupMs - flushMs : 0;
  uint32_t msPerPage = addedThisTick > 0 ? pagingMs / (uint32_t)addedThisTick : 0;
  const char* outcome = completed ? "done" : "interrupted";
  IPLOG("%s: pages=%d (+%d this tick) lastOffset=%u target=%u "
        "elapsed=%ums [setup=%ums paging=%ums (~%ums/page) flush=%ums]",
        outcome, count, addedThisTick, lastOffset, targetOffset,
        elapsedMs, setupMs, pagingMs, msPerPage, flushMs);
  if (completed) recordCompletedGoal(path, layout, targetOffset, targetPage);
  if (prevCpuMhz < 240) setCpuFrequencyMhz(prevCpuMhz);
  return didAnyWork;
}

}  // namespace IdlePaginator
