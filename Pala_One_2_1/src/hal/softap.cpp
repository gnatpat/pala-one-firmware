#include "src/hal/softap.h"

#include <WiFi.h>
#include <esp_bt.h>
#include <esp_wifi.h>

#include "src/state.h"  // AP_SSID, AP_PASS

SoftApInfo softApBegin() {
  setCpuFrequencyMhz(240);  // WiFi AP needs full speed

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  IPAddress ip = WiFi.softAPIP();

  return SoftApInfo{
    AP_SSID,
    AP_PASS,
    String("http://") + ip.toString(),
  };
}

void softApEnd() {
  WiFi.softAPdisconnect(true);
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
  delay(100);
  esp_wifi_stop();
  btStop();
  setCpuFrequencyMhz(80);  // back to low-power idle
}
