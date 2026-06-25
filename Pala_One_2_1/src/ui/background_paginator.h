#ifndef PALA_UI_BACKGROUND_PAGINATOR_H
#define PALA_UI_BACKGROUND_PAGINATOR_H

#include <Arduino.h>

// ============================================================================
//  BackgroundPaginator — pre-builds the on-disk page-offset cache for the
//  most-recently-opened book so that opening it again is instant.
//
//  The motivation: when the user changes a layout-affecting setting (font
//  size, family, line spacing, bionic) the disk page cache is invalidated.
//  The next time they open the book, the reader has to walk pagination from
//  page 0 to the saved byte offset — minutes of stutter, deep into a long
//  book. This module fills the cache so re-opening is fast.
//
//  Two drivers, same engine:
//    - The main loop calls `tick()` in the background between other work,
//      so the cache gets quietly refilled while the device sits idle.
//    - The `BookLoadingScreen` calls `tick()` in a tight loop when the user
//      opens a book whose cache *isn't* already caught up — the user sees
//      an honest progress bar instead of a frozen device.
//
//  Scope: we paginate only up to the saved byte offset, not all the way to
//  EOF. The reader paginates the rest lazily as the user advances — that
//  work has to happen either way (to lay out the text for rendering), so
//  pre-doing it would just be wasted work. The "open is slow" pain we're
//  fixing is purely the find-page-for-offset walk from 0; once we cover
//  the read-to-here byte, opening lands instantly and the user continues
//  with no further setup cost than they'd have had anyway.
//
//  Designed to be entirely stateless across ticks:
//    - `pg_recent` NVS key (set by the reader on open) tells us what book.
//    - The cache file's own header tells us how far we've paginated.
//    - `Font::layoutForCache()` tells us the current layout to paginate for.
//  Each `tick()` re-derives all three. No `kickoff()` / `stop()` plumbing,
//  no in-flight state to manage across deep sleep, no cross-module hooks
//  beyond the one-line `saveLastOpenedBook` from the reader's open path.
//
//  Self-gating: returns false immediately if the reader is currently active
//  (the reader paginates lazily as the user advances and bulk-saves the
//  cache on close; competing writers would clobber its work) or if there's
//  no recent book to target.
// ============================================================================
namespace BackgroundPaginator {

// Record `path` as the most-recently-opened book in NVS. Called from the
// reader's open path. Same-path calls are a no-op to keep NVS writes minimal.
void saveLastOpenedBook(const String& path);

// Make pagination progress for up to `budgetMs` milliseconds. Returns true
// iff any actual pagination work was performed (caller can use this to
// decide whether to skip light sleep this iteration to keep making progress).
//
// Returns false (cheaply, microseconds) when:
//   - No `pg_recent` book is registered.
//   - The reader is currently active.
//   - The book file is gone (clears stale `pg_recent`).
//   - Pagination has already reached EOF or MAX_PAGES for this layout.
//
// Aborts mid-batch on `buttonQueueNonEmpty()` so input latency stays bounded
// by one page's pagination cost, not by `budgetMs`.
bool tick(uint32_t budgetMs);

}  // namespace BackgroundPaginator

#endif  // PALA_UI_BACKGROUND_PAGINATOR_H
