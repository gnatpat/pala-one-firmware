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
  uint32_t   lastHostByteMs  = 0;      // 0 = never; bumped whenever the host sends us bytes
};

State s_state;
}  // namespace

// How long after the last byte received from the host we still consider the
// session "active" for the sleep gate. Just having the USB-CDC port open
// isn't enough — a plugged-in laptop or `pio device monitor` shouldn't pin
// the device awake. Long enough to cover a user reading the browser dialog
// between sends; short enough that closing the tab lets sleep resume.
static constexpr uint32_t kActiveAfterByteMs = 30000;

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
  if (!Serial) return false;
  // Active during the brief window where we hold Wi-Fi for a provisioning,
  // OR whenever the host has sent us bytes recently (= browser is actually
  // talking to us, not just holding the port open).
  if (s_state.ownsWifi) return true;
  if (s_state.lastHostByteMs != 0
      && (uint32_t)(millis() - s_state.lastHostByteMs) < kActiveAfterByteMs) {
    return true;
  }
  return false;
}

void loop() {
  if (!Serial) {
    // Host disconnected. Drop anything we'd been holding.
    releaseWifi();
    clearGraceTimer();
    s_state.lastHostByteMs = 0;
    return;
  }

  // Note recent activity *before* handleSerial drains the buffer, so the
  // sleep gate keeps us awake for the next ~30s of dialog idle time.
  if (Serial.available() > 0) {
    s_state.lastHostByteMs = millis();
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
