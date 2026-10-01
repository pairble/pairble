/*
 * pairble for Arduino: glue.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Both definitions here replace weak ones, which needs the library linked as
 * object files (libArchive: false in library.json; the Arduino IDE does that
 * anyway).
 */
#include <WiFi.h>
#include <esp_wifi.h>

// Arduino releases the Bluetooth memory at startup unless the application
// says it uses BLE (a weak bleInUse() in esp32-hal-bt.c); pairble's first
// start would then crash the controller ("HLI Magic mismatch").
extern "C" bool bleInUse(void)
{
  return true;
}

// Arduino's WiFi object keeps its own record of whether Wi-Fi is up. Started
// behind its back, the first WiFi.* call that needs it - scanNetworks(),
// mode() - initialises Wi-Fi a second time and aborts on the interfaces that
// exist already. So Arduino sets Wi-Fi up, its way (interfaces, events),
// and stops it again: pairble starts it, and WiFi.status(), WiFi.SSID(),
// WiFi.localIP(), WiFi.scanNetworks() all work. Arduino's own reconnecting
// is off: pairble reconnects, with its own back-off.
//
// Except one: Arduino's STA handler reconnects once after the first lost
// connection of a boot whatever setAutoReconnect() says (`first_connect` in
// STA.cpp). Behind pairble's back that turns a wrong password into "network
// not found" (its disconnect ends the attempt) and writes the station config,
// RAM-only hints included, to flash. A made-up disconnect while Wi-Fi is
// stopped uses it up harmlessly: its disconnect() and connect() fail.
static bool waitForStatus(wl_status_t status)
{
  for (int waited = 0; waited < 1000; waited += 10)
  {
    if (WiFi.status() == status)
      return true;
    delay(10);
  }
  return false;
}

extern "C" void pairble_platform_wifi_init(void)
{
  WiFi.setAutoReconnect(false);
  WiFi.mode(WIFI_STA);
  esp_wifi_stop();
  // Arduino has seen the stop (so a late STA_STOP cannot reach pairble's
  // handlers, registered next, and cancel what they arm).
  waitForStatus(WL_STOPPED);

  arduino_event_t ev = {};
  ev.event_id = ARDUINO_EVENT_WIFI_STA_DISCONNECTED;
  ev.event_info.wifi_sta_disconnected.reason = WIFI_REASON_UNSPECIFIED;
  Network.postEvent(&ev);
  waitForStatus(WL_DISCONNECTED);
}
