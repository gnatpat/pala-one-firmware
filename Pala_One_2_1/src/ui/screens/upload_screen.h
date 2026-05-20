#ifndef PALA_UI_SCREENS_UPLOAD_SCREEN_H
#define PALA_UI_SCREENS_UPLOAD_SCREEN_H

#include "src/hal/softap.h"  // SoftApInfo (cached for draw())
#include "src/ui/screen.h"

class UploadScreen : public Screen {
public:
  void onEnter() override;
  void onButton(const ButtonEvent& e) override;
  void draw() override;
  void onIdleTick() override;

  // The SoftAP can't keep running while the device deep-sleeps.
  bool allowSleep() const override { return false; }

private:
  uint32_t   startedMs_ = 0;   // for the auto-exit timer
  SoftApInfo net_;             // SSID/pass/url shown by draw(); populated in startSession()

  void startSession();
  void stopSessionToLibrary();
};

extern UploadScreen g_uploadScreen;

#endif  // PALA_UI_SCREENS_UPLOAD_SCREEN_H
