#include "src/hal/wifi.h"

#include <ESPmDNS.h>
#include <WiFi.h>
#include <esp_bt.h>
#include <esp_wifi.h>

#include "src/state.h"               // AP_SSID, AP_PASS
#include "src/storage/wifi_creds.h"

static constexpr const char* kMdnsHost = "pala-one";

static WifiSession beginAccessPoint() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  IPAddress ip = WiFi.softAPIP();

  WifiSession s;
  s.mode       = WifiMode::AccessPoint;
  s.apSsid     = AP_SSID;
  s.apPass     = AP_PASS;
  s.primaryUrl = String("http://") + ip.toString();
  return s;
}

// Returns true and fills `out` on a successful STA association within
// staTimeoutMs. Returns false on timeout or user cancel; caller falls back
// to AP. Leaves WiFi in a clean state on failure (disconnected, mode reset).
static bool tryStation(uint32_t staTimeoutMs, bool (*cancel)(), WifiSession& out) {
  if (!WifiCreds::has()) return false;

  const String ssid = WifiCreds::ssid();
  const String pass = WifiCreds::pass();

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());

  const uint32_t start = millis();
  while ((uint32_t)(millis() - start) < staTimeoutMs) {
    if (WiFi.status() == WL_CONNECTED) {
      IPAddress ip = WiFi.localIP();
      MDNS.begin(kMdnsHost);
      MDNS.addService("http", "tcp", 80);

      out.mode        = WifiMode::Station;
      out.staSsid     = ssid;
      out.primaryUrl  = String("http://") + kMdnsHost + ".local";
      out.fallbackUrl = String("http://") + ip.toString();
      return true;
    }
    if (cancel && cancel()) break;
    delay(50);
  }

  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
  delay(20);
  return false;
}

WifiSession wifiBeginUploadSession(uint32_t staTimeoutMs, bool (*cancel)()) {
  setCpuFrequencyMhz(240);  // WiFi (either mode) needs full speed

  WifiSession s;
  if (tryStation(staTimeoutMs, cancel, s)) return s;
  return beginAccessPoint();
}

void wifiEnd() {
  MDNS.end();
  WiFi.softAPdisconnect(true);
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
  delay(100);
  esp_wifi_stop();
  btStop();
  setCpuFrequencyMhz(80);  // back to low-power idle
}
