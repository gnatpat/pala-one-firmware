#ifndef PALA_UI_SCREENS_UPLOAD_SCREEN_H
#define PALA_UI_SCREENS_UPLOAD_SCREEN_H

#include "src/hal/wifi.h"    // WifiSession (cached for draw())
#include "src/ui/screen.h"

class UploadScreen : public Screen {
public:
  void onEnter() override;
  void onButton(const ButtonEvent& e) override;
  void draw() override;
  void onIdleTick() override;

  // The Wi-Fi session (AP or STA) can't keep running while the device
  // deep-sleeps.
  bool allowSleep() const override { return false; }

private:
  uint32_t    startedMs_ = 0;   // for the auto-exit timer
  WifiSession net_;             // cached session info shown by draw()

  void startSession();
  void stopSessionToLibrary();
};

extern UploadScreen g_uploadScreen;

#endif  // PALA_UI_SCREENS_UPLOAD_SCREEN_H
