#include "src/hal/improv.h"

#include <ImprovWiFiLibrary.h>
#include <WiFi.h>

#include "src/storage/wifi_creds.h"

namespace Improv {

namespace {
struct State {
  ImprovWiFi lib{&Serial};
  bool       uploadSession   = false;
  bool       credsReceived   = false;
  uint32_t   credsReceivedMs = 0;
  bool       ownsWifi        = false;  // true iff Improv brought Wi-Fi up to verify creds
};

State s_state;
}  // namespace

// After the credentials callback fires we give the library this much time to
// flush its "Provisioned" reply over USB-CDC before tearing Wi-Fi down. The
// reply itself takes microseconds at 115200 baud; this is just headroom.
static constexpr uint32_t kShutdownGraceMs = 2000;

static void releaseWifi() {
  if (!s_state.ownsWifi) return;
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
  s_state.ownsWifi = false;
}

static void clearGraceTimer() {
  s_state.credsReceived   = false;
  s_state.credsReceivedMs = 0;
}

void begin() {
  s_state.lib.setDeviceInfo(
      ImprovTypes::ChipFamily::CF_ESP32_S3,
      "Pala One",
      "2.1",
      "Pala One");

  // The library handles WiFi.begin() itself; we only persist creds once the
  // association succeeds. If the association fails the library reports the
  // error back to the browser and the saved creds stay untouched, which is
  // exactly what we want (don't poison NVS with bad creds).
  //
  // Known edge case (only when an upload session is active): the library
  // calls WiFi.mode(WIFI_STA)+begin() internally, which kills the upload
  // session's SoftAP and disconnects any phone mid-upload. Considered narrow
  // enough to defer; fix would be `setCustomConnectWiFi(...)` to bypass the
  // lib's connect while a session is up and just save creds.
  s_state.lib.onImprovConnected([](const char* ssid, const char* password) {
    WifiCreds::save(ssid, password);
    if (!s_state.uploadSession) {
      // We brought Wi-Fi up just to verify creds — schedule teardown after
      // the library finishes sending its "Provisioned" reply.
      s_state.ownsWifi        = true;
      s_state.credsReceived   = true;
      s_state.credsReceivedMs = millis();
    }
    // Upload session: leave Wi-Fi alone — upload screen owns it.
  });
}

void notifyUploadSession(bool active) {
  s_state.uploadSession = active;
}

bool isActive() {
  return static_cast<bool>(Serial);
}

void loop() {
  if (!Serial) {
    // Host disconnected. Drop anything we'd been holding.
    releaseWifi();
    clearGraceTimer();
    return;
  }

  if (s_state.credsReceived
      && (uint32_t)(millis() - s_state.credsReceivedMs) > kShutdownGraceMs) {
    releaseWifi();
    clearGraceTimer();
    // Keep listening — host might want to re-provision a different network.
  }

  s_state.lib.handleSerial();
}

}  // namespace Improv
