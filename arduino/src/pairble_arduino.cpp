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
extern "C" void pairble_platform_wifi_init(void)
{
  WiFi.setAutoReconnect(false);
  WiFi.mode(WIFI_STA);
  esp_wifi_stop();
}
