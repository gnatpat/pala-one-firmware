#include "src/web/apps_upload.h"

#include "src/config.h"                       // MAX_APPS, MAX_APP_BINARY
#include "pala_app.h"                     // PalaAppHeader, PALA_APP_MAGIC
#include "src/pure/app_header.h"              // validateAppHeader / status enum
#include "src/pure/paths.h"                   // sanitizeUploadedAppFilename
#include "src/state.h"                        // FS, server
#include "src/storage/app_catalog.h"          // g_apps, loadApps
#include "src/storage/fs_util.h"              // fsFreeBytesSafe
#include "src/web/chrome.h"                   // successPage, htmlEscape, humanBytes

// ============================================================================
//  Per-session state. File-static because the route handlers below are the
//  only readers; the screen reaches it only through resetAppUpload().
// ============================================================================
namespace {
struct AppUpload {
  File   tmpFile;
  String tmpPath;
  String finalName;
  bool   ok = false;
  String error;
};

AppUpload s_app;
}  // namespace

void resetAppUpload() {
  if (s_app.tmpFile) s_app.tmpFile.close();
  s_app = AppUpload{};
}

// ============================================================================
//  App .bin upload — single-pass streaming receiver.
//
//  Files land in /apps/<sanitized>.bin.tmp during the transfer; on
//  UPLOAD_FILE_END we read the first sizeof(PalaAppHeader) bytes back
//  off the temp file and run them through `validateAppHeader`. If that
//  passes, we rename the temp into place and refresh `g_apps` so the
//  on-device AppsScreen sees the new entry without a reboot.
//
//  Header validation here mirrors what the on-device loader does
//  (Stage 6). The intent is "fail at upload time, not at launch time" —
//  if the magic or api_version is wrong, we'd rather the browser show
//  the error now than have a useless entry sit in the launcher.
// ============================================================================

static void handleUploadAppDone() {
  AppUpload& s = s_app;
  if (!s.ok) {
    server.send(400, "text/plain; charset=utf-8",
                s.error.length() ? s.error : String("App upload failed"));
    return;
  }

  loadApps();  // refresh so AppsScreen sees the new entry on next entry

  String finalPath = String("/apps/") + s.finalName;
  size_t storedSize = 0;
  File stored = FS.open(finalPath, "r");
  if (stored) { storedSize = stored.size(); stored.close(); }

  String inner;
  inner.reserve(900);
  inner += "<div class='card'><h2>App installed</h2>";
  inner += "<p class='muted'>Open the device, scroll the library to <b>Apps</b>, and double-click to launch.</p>";
  inner += "<div class='stats'>";
  inner += "<div class='stat'><span class='muted'>App</span><b>"        + htmlEscape(s.finalName)      + "</b></div>";
  inner += "<div class='stat'><span class='muted'>Stored size</span><b>"+ humanBytes(storedSize)        + "</b></div>";
  inner += "<div class='stat'><span class='muted'>Apps now</span><b>"   + String(g_apps.count)          + "</b></div>";
  inner += "<div class='stat'><span class='muted'>Free space</span><b>" + humanBytes(fsFreeBytesSafe()) + "</b></div>";
  inner += "</div><div class='actions'><a class='btn' href='/'>Upload another</a></div></div>";
  inner += storageCardHtml();

  String page = successPage(
    "App installed",
    "App saved to /apps/.",
    "&#10003; App ready to run.",
    inner
  );
  server.send(200, "text/html; charset=utf-8", page);
}

// Map an AppHeaderStatus into a user-facing message. Mirrors the
// runApp switch in ui/pala_api_impl.cpp; lives here so the upload route
// can present the same diagnostics at install time.
static const char* validationErrorMessage(AppHeaderStatus s, uint32_t fileApiVer) {
  static char buf[48];
  switch (s) {
    case AppHeaderStatus::Ok:             return "OK";
    case AppHeaderStatus::TooSmall:       return "Invalid app (file too small)";
    case AppHeaderStatus::BadMagic:       return "Invalid app (bad magic)";
    case AppHeaderStatus::BadEntryOffset: return "Invalid app (bad entry offset)";
    case AppHeaderStatus::BadRelocTable:  return "Invalid app (bad reloc table)";
    case AppHeaderStatus::ApiMismatch:
      snprintf(buf, sizeof(buf), "Invalid app (API v%u, need v%u)",
               (unsigned)fileApiVer, (unsigned)PALA_API_VERSION);
      return buf;
  }
  return "Invalid app";
}

static void handleUploadAppStream() {
  AppUpload& s = s_app;
  HTTPUpload& up = server.upload();

  if (up.status == UPLOAD_FILE_START) {
    s.ok = false;
    s.error = "";
    s.finalName = "";
    s.tmpPath = "";

    // Defensive — protects MAX_APPS check from a stale catalog.
    loadApps();
    if (g_apps.count >= MAX_APPS) {
      s.error = "Apps directory full";
      return;
    }

    // Pre-check free space against the maximum app size — we don't know
    // the upload's final size yet, but we know it can't exceed
    // MAX_APP_BINARY. Lets us fail fast if storage is nearly full
    // rather than burning the whole transfer.
    if (fsFreeBytesSafe() < MAX_APP_BINARY) {
      s.error = "Not enough free space";
      return;
    }

    // Make sure /apps/ exists; loadApps creates it but we may have
    // bailed out above.
    if (!FS.exists("/apps")) FS.mkdir("/apps");

    s.finalName = sanitizeUploadedAppFilename(up.filename);
    s.tmpPath   = String("/apps/") + s.finalName + ".tmp";

    if (FS.exists(s.tmpPath)) FS.remove(s.tmpPath);
    s.tmpFile = FS.open(s.tmpPath, "w");
    if (!s.tmpFile) {
      s.error = "Cannot create temp app file";
      s.tmpPath = "";
    }
  }
  else if (up.status == UPLOAD_FILE_WRITE) {
    if (s.error.length() > 0) return;
    if (!s.tmpFile) return;

    // Hard cap on cumulative size during streaming so a runaway upload
    // can't fill the partition before END fires.
    if (s.tmpFile.size() + up.currentSize > MAX_APP_BINARY) {
      s.tmpFile.close();
      if (FS.exists(s.tmpPath)) FS.remove(s.tmpPath);
      s.tmpPath = "";
      s.error = "App too large (> 48 KB)";
      return;
    }
    s.tmpFile.write(up.buf, up.currentSize);
  }
  else if (up.status == UPLOAD_FILE_END) {
    if (s.tmpFile) s.tmpFile.close();
    if (s.error.length() > 0 || s.tmpPath.length() == 0) return;

    if (up.totalSize < sizeof(PalaAppHeader) + 4) {
      if (FS.exists(s.tmpPath)) FS.remove(s.tmpPath);
      s.error = "App binary too small";
      s.tmpPath = "";
      return;
    }

    // Read the header back and validate it with the same pure routine
    // the on-device loader uses, so install-time and load-time agree on
    // what counts as a valid app.
    uint8_t hdrBuf[sizeof(PalaAppHeader)] = {0};
    File vf = FS.open(s.tmpPath, "r");
    bool readOk = false;
    if (vf) {
      readOk = (vf.read(hdrBuf, sizeof(hdrBuf)) == (int)sizeof(hdrBuf));
      vf.close();
    }
    if (!readOk) {
      if (FS.exists(s.tmpPath)) FS.remove(s.tmpPath);
      s.error = "Could not read app header";
      s.tmpPath = "";
      return;
    }

    uint32_t fileApiVer = 0;
    // We only have the header bytes here, but `validateAppHeader` only
    // touches header fields. Pass the *real* file size so the
    // entry/reloc range checks operate against the true bounds.
    AppHeaderStatus st = validateAppHeader(hdrBuf, (size_t)up.totalSize, &fileApiVer);
    if (st != AppHeaderStatus::Ok) {
      if (FS.exists(s.tmpPath)) FS.remove(s.tmpPath);
      s.error = String(validationErrorMessage(st, fileApiVer));
      s.tmpPath = "";
      return;
    }

    // Commit: atomic rename into the canonical /apps/<name>.bin path.
    String finalPath = String("/apps/") + s.finalName;
    if (FS.exists(finalPath)) FS.remove(finalPath);
    if (FS.rename(s.tmpPath, finalPath)) {
      s.ok = true;
    } else {
      if (FS.exists(s.tmpPath)) FS.remove(s.tmpPath);
      s.error = "Failed to finalize app upload";
    }
    s.tmpPath = "";
  }
  else if (up.status == UPLOAD_FILE_ABORTED) {
    if (s.tmpFile) s.tmpFile.close();
    if (s.tmpPath.length() > 0 && FS.exists(s.tmpPath)) FS.remove(s.tmpPath);
    s.tmpPath = "";
    s.ok = false;
    s.error = "App upload aborted";
  }
}

static void handleDeleteApp() {
  if (!server.hasArg("name")) {
    server.send(400, "text/plain; charset=utf-8", "missing name");
    return;
  }
  String name = server.arg("name");
  if (name.indexOf('/') >= 0 || name.indexOf('\\') >= 0 || !name.endsWith(".bin")) {
    server.send(400, "text/plain; charset=utf-8", "invalid name");
    return;
  }
  String path = String("/apps/") + name;
  if (FS.exists(path)) FS.remove(path);
  loadApps();   // refresh catalog so the next read sees the deletion

  server.sendHeader("Location", "/files");
  server.send(303);
}

void registerAppUploadRoutes() {
  server.on("/upload-app", HTTP_POST, handleUploadAppDone, handleUploadAppStream);
  server.on("/del-app",    HTTP_POST, handleDeleteApp);   // POST: destructive
}
