#ifndef PALA_HAL_WIFI_H
#define PALA_HAL_WIFI_H

#include <Arduino.h>

// Bundled "upload-mode" power+net resource: brings Wi-Fi up for the upload
// session and clocks the CPU to 240 MHz; wifiEnd() reverses both. Only one
// caller today (the upload screen) so the CPU-freq concern rides along; split
// if a second caller needs just the radio without the clock change.
//
// Tries Station (STA) mode first if creds are stored ([[wifi-creds]]), with a
// timeout and an early-cancel hook so the upload screen can let the user bail
// out and use the hotspot. On STA failure / cancel / no creds, falls back to
// the original SoftAP behaviour byte-for-byte.

enum class WifiMode { Station, AccessPoint };

struct WifiSession {
  WifiMode    mode = WifiMode::AccessPoint;
  String      primaryUrl;     // STA: http://pala-one.local  AP: http://192.168.4.1
  String      fallbackUrl;    // STA: http://<lan-ip>        AP: ""
  const char* apSsid = "";    // populated in AccessPoint mode only
  const char* apPass = "";    // populated in AccessPoint mode only
  String      staSsid;        // populated in Station mode only
};

// `cancel` is polled while waiting for the STA association — returning true
// aborts the attempt and triggers AP fallback. Pass nullptr to disable.
WifiSession wifiBeginUploadSession(uint32_t staTimeoutMs, bool (*cancel)());
void        wifiEnd();

#endif  // PALA_HAL_WIFI_H
