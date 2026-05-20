#include "src/ui/screens/upload_screen.h"

#include "src/config.h"           // BTN
#include "src/hal/display.h"
#include "src/hal/improv.h"
#include "src/hal/wifi.h"
#include "src/state.h"            // server
#include "src/storage/library.h"
#include "src/storage/wifi_creds.h"
#include "src/ui/font.h"
#include "src/ui/screens/library_screen.h"
#include "src/ui/widgets.h"
#include "src/web/apps_upload.h"  // resetAppUpload()
#include "src/web/upload.h"       // resetBookUpload() / resetSleepUpload()

// ---- Cancel callback ------------------------------------------------------
// The STA-association poll inside wifiBeginUploadSession() calls this every
// ~50ms. We watch for a button press, but only after the button has first
// gone HIGH — otherwise the click that brought the user *to* the upload
// screen would itself be read as a cancel.
static bool s_cancelButtonReleased = false;

static bool cancelOnButtonPress() {
  const int btn = digitalRead(BTN);
  if (btn == HIGH) s_cancelButtonReleased = true;
  return s_cancelButtonReleased && (btn == LOW);
}

// ---- Connecting splash ----------------------------------------------------
// Shown briefly while we try STA, so the user has something to look at and
// knows they can bail out.
static void drawConnecting(const String& ssid) {
  prepareMenuFrame();
  int y = drawSectionHeader("Upload");

  Font::useBold();
  u8g2.setCursor(MARGIN_X, y);
  u8g2.print("Connecting");
  y += 14;

  Font::useBody();
  u8g2.setCursor(MARGIN_X, y);
  u8g2.print(ssid.c_str());
  y += 18;

  Font::useBody();
  u8g2.setCursor(MARGIN_X, y);
  u8g2.print("Press button to");
  y += 14;
  u8g2.setCursor(MARGIN_X, y);
  u8g2.print("use hotspot instead");

  display.update();
}

void UploadScreen::onEnter() {
  startSession();
  draw();
}

void UploadScreen::draw() {
  prepareMenuFrame();

  int y = drawSectionHeader("Upload");

  if (net_.mode == WifiMode::Station) {
    Font::useBold();
    u8g2.setCursor(MARGIN_X, y);
    u8g2.print("Connected");
    y += 14;

    Font::useBody();
    u8g2.setCursor(MARGIN_X, y);
    u8g2.print(net_.staSsid.c_str());
    y += 18;

    Font::useBold();
    u8g2.setCursor(MARGIN_X, y);
    u8g2.print("Open");
    y += 14;

    Font::useBody();
    u8g2.setCursor(MARGIN_X, y);
    u8g2.print(net_.primaryUrl.c_str());
    y += 14;

    if (net_.fallbackUrl.length() > 0) {
      u8g2.setCursor(MARGIN_X, y);
      u8g2.print(net_.fallbackUrl.c_str());
      y += 14;
    }
  } else {
    Font::useBold();
    u8g2.setCursor(MARGIN_X, y);
    u8g2.print("Wi-Fi");
    y += 14;

    Font::useBody();
    u8g2.setCursor(MARGIN_X, y);
    u8g2.print(net_.apSsid);
    y += 16;

    Font::useBold();
    u8g2.setCursor(MARGIN_X, y);
    u8g2.print("Password");
    y += 14;

    Font::useBody();
    u8g2.setCursor(MARGIN_X, y);
    u8g2.print(net_.apPass);
    y += 16;

    Font::useBold();
    u8g2.setCursor(MARGIN_X, y);
    u8g2.print("Open");
    y += 14;

    Font::useBody();
    u8g2.setCursor(MARGIN_X, y);
    u8g2.print(net_.primaryUrl.c_str());
    y += 18;
  }

  display.update();
}

void UploadScreen::startSession() {
  startedMs_ = millis();
  resetBookUpload();
  resetSleepUpload();
  resetAppUpload();

  // If we have stored creds, give the user a connecting splash + 5s STA
  // window before falling back. Without creds, go straight to AP — no point
  // showing a connecting screen for a path we know will time out.
  if (WifiCreds::has()) {
    drawConnecting(WifiCreds::ssid());
    s_cancelButtonReleased = false;
    net_ = wifiBeginUploadSession(5000, cancelOnButtonPress);
  } else {
    net_ = wifiBeginUploadSession(0, nullptr);
  }

  server.begin();

  // Improv is always polling whenever USB is connected; just tell it not to
  // touch Wi-Fi while we hold the radio. (Important because there's no
  // physical reset button on the device, so users may need to reprovision
  // mid-session by plugging in USB.)
  Improv::notifyUploadSession(true);
}

void UploadScreen::stopSessionToLibrary() {
  Improv::notifyUploadSession(false);
  server.stop();
  wifiEnd();
  resetBookUpload();
  resetSleepUpload();
  resetAppUpload();

  loadBooks();
  resetInputFrontend();
  nextScreen = &g_libraryScreen;
}

void UploadScreen::onButton(const ButtonEvent& e) {
  if (e.kind == ButtonEvent::Short || e.kind == ButtonEvent::Triple) {
    stopSessionToLibrary();
  }
}

void UploadScreen::onIdleTick() {
  server.handleClient();
  if ((uint32_t)(millis() - startedMs_) > UPLOAD_AUTO_EXIT_MS) {
    stopSessionToLibrary();
  }
}
