#include "src/ui/screens/book_loading_screen.h"

#include "src/config.h"                          // MARGIN_X, SCREEN_W, SCREEN_H
#include "src/hal/display.h"                     // u8g2, gfx, display
#include "src/pure/hashing.h"                    // prefKeyForBook
#include "src/pure/paths.h"                      // lastPathComponent, stripTxtExt
#include "src/state.h"                           // prefs
#include "src/storage/book_metadata.h"           // loadSavedOffset, kOffsetUnset
#include "src/storage/library.h"                 // bookPath
#include "src/storage/page_cache.h"              // readCacheProgress, pageCacheCoversOffset
#include "src/storage/preferences_store.h"
#include "src/ui/background_paginator.h"         // saveLastOpenedBook
#include "src/ui/font.h"
#include "src/ui/reader.h"                       // openBookByIndex
#include "src/ui/screens/library_screen.h"       // navigateToLibraryRoot
#include "src/ui/screens/reader_screen.h"        // g_readerScreen
#include "src/ui/widgets.h"                      // prepareMenuFrame, drawSectionHeader, drawCenter

// `g_bookLoadingScreen` is defined alongside the other screen instances in
// Pala_One_2_1.ino, matching the convention used by every other screen.

// Per-instance state cached at onEnter — saves re-deriving them on every
// onIdleTick + draw. The book itself is identified by `bookIdx` (set by
// the library before transitioning to us); the rest fall out of that.
static String   s_path         = "";
static String   s_bookName     = "";
static uint32_t s_targetOffset = 0;
// Last percentage we drew, used to skip redundant e-ink refreshes when
// pagination hasn't made enough progress to move the bar visibly.
static int      s_lastDrawnPct = -1;

bool BookLoadingScreen::isNeededFor(int bookIdx) {
  const char* p = bookPath(bookIdx);
  if (!p) return false;
  String path(p);

  PreferencesStore kv(prefs);
  uint32_t targetOffset = loadSavedOffset(kv, prefKeyForBook(path));
  // Never-read books (or pre-offset firmware data) skip the loading
  // screen — opening is already instant since there's no pagination to do.
  if (targetOffset == kOffsetUnset || targetOffset == 0) return false;

  return !pageCacheCoversOffset(path, Font::layoutForCache(), targetOffset);
}

void BookLoadingScreen::onEnter() {
  // Cache the things draw() and onIdleTick() need each frame, so we don't
  // re-derive them per loop iteration.
  const char* p = bookPath(bookIdx);
  s_path = p ? String(p) : "";
  s_bookName = stripTxtExt(lastPathComponent(s_path));

  PreferencesStore kv(prefs);
  s_targetOffset = loadSavedOffset(kv, prefKeyForBook(s_path));
  s_lastDrawnPct = -1;

  // Same hook the reader uses on open — tells the background paginator
  // which book to work on, so the main loop's per-iteration `tick()` call
  // is targeting us. We're not calling tick() ourselves: the main loop
  // already does, and adding a second driver would double the budget
  // per loop iteration and slow the visible progress.
  BackgroundPaginator::saveLastOpenedBook(s_path);

  draw();
}

void BookLoadingScreen::onButton(const ButtonEvent& e) {
  if (!e.any()) return;
  // Any press cancels and returns to where the user came from. The
  // background paginator keeps running on its own afterwards, so the
  // user's progress isn't lost.
  navigateToLibraryRoot();
}

// Read current cache state. Returns the percentage [0..100] of the saved
// offset that's been paginated so far. Sets `*outCovers = true` once the
// cache fully covers the saved offset (i.e., we're done loading).
static int readProgressPct(bool* outCovers) {
  uint16_t count = 0;
  uint32_t lastOffset = 0;
  uint32_t storedSize = 0;
  bool ok = readCacheProgress(s_path, Font::layoutForCache(),
                              &count, &lastOffset, &storedSize);
  if (!ok) { *outCovers = false; return 0; }

  *outCovers = (lastOffset >= s_targetOffset);
  if (s_targetOffset == 0) return 100;
  uint32_t pct = (uint32_t)((uint64_t)lastOffset * 100ULL / (uint64_t)s_targetOffset);
  if (pct > 100) pct = 100;
  return (int)pct;
}

void BookLoadingScreen::draw() {
  prepareMenuFrame();
  Font::useBody();
  int ascent = u8g2.getFontAscent();
  int lineH = (ascent - u8g2.getFontDescent()) + Font::currentLineGap() + 1;
  int y = drawSectionHeader("Preparing book");

  Font::useBold();
  u8g2.setCursor(MARGIN_X, y);
  u8g2.print(s_bookName.c_str());
  y += lineH;
  Font::useBody();

  bool covers = false;
  int pct = readProgressPct(&covers);

  // Progress bar centred horizontally, fills left-to-right.
  int barW = SCREEN_W - 2 * MARGIN_X;
  int barH = 10;
  int barY = y + 4;
  int filled = (barW * pct) / 100;
  if (filled < 0) filled = 0;
  if (filled > barW) filled = barW;
  gfx.drawRect(MARGIN_X, barY, barW, barH, 1);
  if (filled > 2) gfx.fillRect(MARGIN_X + 1, barY + 1, filled - 2, barH - 2, 1);
  y = barY + barH + lineH;

  char buf[48];
  snprintf(buf, sizeof(buf), "%d%%", pct);
  u8g2.setCursor(MARGIN_X, y);
  u8g2.print(buf);

  u8g2.setCursor(MARGIN_X, SCREEN_H - 2);
  u8g2.print("click: cancel");

  display.update();
  s_lastDrawnPct = pct;
}

void BookLoadingScreen::onIdleTick() {
  // The main loop already calls BackgroundPaginator::tick once per
  // iteration with full CPU — we just observe progress and decide when
  // to hand off. No paginator calls here means a single driver, no
  // doubled-up budget per loop.
  bool covers = false;
  int pct = readProgressPct(&covers);

  if (covers) {
    // Caught up. Open the book — fast cache path applies now — and
    // transition to the reader.
    if (openBookByIndex(bookIdx)) {
      nextScreen = &g_readerScreen;
    } else {
      // Book disappeared mid-load (deleted via web UI, for example).
      drawCenter("Book unavailable", "Returning to library");
      navigateToLibraryRoot();
    }
    return;
  }

  // Only redraw when the percentage actually moved — e-ink refresh isn't
  // free, and the bar wouldn't look any different otherwise.
  if (pct != s_lastDrawnPct) draw();
}
