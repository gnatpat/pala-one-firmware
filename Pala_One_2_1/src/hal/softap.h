#ifndef PALA_HAL_SOFTAP_H
#define PALA_HAL_SOFTAP_H

#include <Arduino.h>

// Bundled "upload-mode" power+net resource: brings up the SoftAP and clocks
// the CPU to 240 MHz; end() reverses both. Only one caller today (the upload
// screen) so the CPU-freq concern rides along; split if a second caller needs
// just the AP without the clock change.
struct SoftApInfo {
  const char* ssid;
  const char* pass;
  String      url;   // e.g. "http://192.168.4.1"
};

SoftApInfo softApBegin();
void       softApEnd();

#endif  // PALA_HAL_SOFTAP_H
