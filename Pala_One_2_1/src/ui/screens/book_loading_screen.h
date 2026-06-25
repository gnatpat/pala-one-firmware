#ifndef PALA_UI_SCREENS_BOOK_LOADING_SCREEN_H
#define PALA_UI_SCREENS_BOOK_LOADING_SCREEN_H

#include "src/ui/screen.h"

// ============================================================================
//  BookLoadingScreen — bridges the gap between "user picks a book" and
//  "reader can open it instantly" when the on-disk page cache isn't yet
//  caught up to the user's saved byte offset (typically: layout changed
//  since the cache was last built).
//
//  Drives `BackgroundPaginator::tick` via the main loop, renders an honest
//  progress bar, and transitions to the reader once the cache covers the
//  saved offset. The screen is otherwise inert — it's a UI shell over the
//  same pagination engine the background path uses.
//
//  Routing: `isNeededFor(bookIdx)` answers "would direct open be slow?" so
//  the library can skip this screen entirely for books whose cache is
//  already current (and for never-read books, where opening is instant).
// ============================================================================
class BookLoadingScreen : public Screen {
public:
  // Set by the caller (library screen) before transitioning here.
  int bookIdx = -1;

  void onEnter() override;
  void onButton(const ButtonEvent& e) override;
  void draw() override;
  void onIdleTick() override;

  // We want full CPU throughput here — the main loop's light-sleep gate
  // already skips sleep when the paginator did work, but in case it ever
  // doesn't, this also guarantees we stay awake during loading.
  bool allowSleep() const override { return false; }

  // Does opening `bookIdx` need this screen, or can the library open
  // directly via openBookByIndex? Cheap (one cache header read + one
  // NVS lookup — no book file open). Returns false for never-read books
  // and books whose cache already covers the saved offset.
  static bool isNeededFor(int bookIdx);
};

extern BookLoadingScreen g_bookLoadingScreen;

#endif  // PALA_UI_SCREENS_BOOK_LOADING_SCREEN_H
