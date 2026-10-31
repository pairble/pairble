# pairble for Arduino

Wi-Fi setup for ESP32 and the Raspberry Pi Pico W over Bluetooth, with nothing to install: the phone's
camera opens the pairble App Clip (iPhone) or a web page (Android, computers
with Bluetooth), the device shows the networks it can see, and the password
travels over an encrypted Bluetooth session. Phones that cannot use Bluetooth
join the device's own Wi-Fi instead. See [pairble.com](https://pairble.com).

On the ESP32 this is the ESP-IDF component [pairble](https://github.com/pairble/pairble),
packaged for Arduino. It needs **arduino-esp32 3.3** or newer (ESP-IDF 5.5); in
PlatformIO that is the [pioarduino](https://github.com/pioarduino/platform-espressif32)
platform - the stock `espressif32` platform still ships arduino-esp32 2.x.

On the **Raspberry Pi Pico W and Pico 2 W** ([arduino-pico](https://github.com/earlephilhower/arduino-pico))
it is an implementation of its own, with the same protocol - the same App
Clip, page and QR codes - and the same API: one sketch builds for both. See
[Pico W](#pico-w) below.

## Install

- **Arduino IDE:** Tools > Manage Libraries, search "pairble". NimBLE-Arduino
  comes with it.
- **PlatformIO:** `lib_deps = pairble/pairble-arduino` (full example below).

## Use

```cpp
#include <pairble.h>

void setup()
{
  Serial.begin(115200);
  if (pairble_start(NULL) != PAIRBLE_OK)
    Serial.println("pairble did not start");
  Serial.printf("QR code: %s\n", pairble_setup_url());
}

void loop()
{
  pairble_loop();  // does nothing on an ESP32; the Pico W needs it
}
```

With a network stored the board connects; without one it opens setup and
`pairble_setup_url()` is what goes into the QR code. Arduino's `WiFi` object
knows about the connection - `WiFi.status()`, `WiFi.localIP()`, `WiFi.SSID()`,
`WiFi.RSSI()` and (on the ESP32) `WiFi.scanNetworks()` work, and so do `HTTPClient` and
`WiFiClient`. Just don't connect with `WiFi.begin()`: pairble owns the
station. A network from elsewhere - a keypad, a menu - goes through
`pairble_set_network()`, which keeps the old one until the new one works.

For web installers such as ESP Web Tools, call
`pairbleImprovSerial(Serial)` after `pairble_start()`: the board then speaks
Improv Wi-Fi on that port, with the same safety. Optional arguments are the
URL shown after success (`{ip}` is replaced) and the firmware name and
version the installer displays. Without the call, Improv is not there. On
the ESP32, to switch Wi-Fi off before deep sleep, call
`esp_wifi_stop()` rather than `WiFi.disconnect()`, so it does not count as the
network being gone.

```ini
; platformio.ini
[env:esp32-s3]
platform = https://github.com/pioarduino/platform-espressif32/releases/download/stable/platform-espressif32.zip
framework = arduino
board = esp32-s3-devkitc-1
board_build.partitions = huge_app.csv   ; room to grow; see below
lib_deps = pairble/pairble-arduino
```

Wi-Fi, Bluetooth and setup together take about 1.1-1.2 MB of flash, which
leaves little of the default 1.25 MB app partition for the application: pick
a larger one (Arduino IDE: Tools > Partition Scheme > Huge APP).

Built and tested with arduino-esp32 3.3 (ESP-IDF 5.5) and NimBLE-Arduino 2.5.
The Bluetooth transport is ESP-IDF's, from that release, so the library
refuses to build with an older core.

## Pico W

Pick the board (Raspberry Pi Pico W or Pico 2 W) and set **Tools >
IP/Bluetooth Stack** to "IPv4 + Bluetooth"; without Bluetooth the library
says so at compile time. In PlatformIO:

```ini
; platformio.ini
[env:picow]
platform = https://github.com/maxgerhardt/platform-raspberrypi.git
board = rpipicow
framework = arduino
board_build.core = earlephilhower
build_flags = -D PIO_FRAMEWORK_ARDUINO_ENABLE_BLUETOOTH
lib_deps = pairble/pairble-arduino
```

What differs from the ESP32:

- **Call `pairble_loop()` from `loop()`**, often: there is no task of its
  own, so Wi-Fi, the access point and Improv are run from there (the
  Bluetooth side runs on its own). Events are called from it, and from
  `pairble_set_network()` while it waits: a callback that takes long holds
  all of that up.
- pairble drives Arduino's `WiFi` object - joining, reconnecting the stored
  network, the access point - so never call `WiFi.begin()`, `WiFi.mode()` or
  `WiFi.disconnect()` yourself. Once the board is online `WiFi.localIP()`,
  `WiFiClient` and the rest work.
- The network and the proof of possession are kept in the emulated EEPROM
  (`EEPROM.begin(256)`, its first 168 bytes): a sketch using EEPROM too must
  use the same size and keep clear of them.
- Setup's RAM (about 3 KB of buffers, plus the access point) is given back
  when it closes, so `reboot_after_success` is off by default here.
- Not here: the setup button and power-cycle triggers, `@pairble/cli`'s
  per-unit PoP in flash (a PoP is generated on first boot, or set one in
  `pairble_config_t`), and the ESP32-only menuconfig settings.
  `pairble_improv_parsing()` is here only, so a sketch for both boards
  can't use it.
- Improv sets the network whether setup is open or not (the ESP32 opens
  setup for it), so it brings no STARTED and STOPPED events.
- The Arduino IDE installs NimBLE-Arduino with the library on the Pico W too;
  it is not used there, and not compiled.

Pico W settings are build flags too, with the ESP32's names and, but for
`CONFIG_PAIRBLE_REBOOT_AFTER_SUCCESS`, its defaults:
`CONFIG_PAIRBLE_NAME_PREFIX`, `CONFIG_PAIRBLE_SERVICE_UUID`,
`CONFIG_PAIRBLE_SETUP_URL`, `CONFIG_PAIRBLE_SOFTAP`,
`CONFIG_PAIRBLE_WINDOW_SECONDS`, `CONFIG_PAIRBLE_RECOVERY_AFTER_SECONDS`,
`CONFIG_PAIRBLE_RECOVERY_WINDOW_SECONDS`, `CONFIG_PAIRBLE_REBOOT_AFTER_SUCCESS`
(see `src/pairble_rp2040.h`).

Tested on the Pico W with arduino-pico 6.3; the Pico 2 W builds but has not
been tried on hardware yet.

## Settings

The ESP-IDF version is configured in menuconfig; here every setting has the
same default and can be overridden with a build flag - see
`src/pairble_arduino_config.h` (ESP32) and `src/pairble_rp2040.h` (Pico W):

```ini
build_flags =
    -D CONFIG_PAIRBLE_NAME_PREFIX='"LAMP-"'
    -D CONFIG_PAIRBLE_SETUP_BUTTON_GPIO=0
```

Most can also be set at run time in `pairble_config_t`; see
`pairble_esp32.h` and `pairble_rp2040.h` (`pairble.h` picks the board's).

## Notes

- arduino-esp32 precompiles `espressif/network_provisioning` 1.0.2. This
  library brings its own newer copy (Apache-2.0, `LICENSE.network_provisioning`),
  which replaces it at link time.
- Bluetooth runs on [NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino)
  on every chip, also on the original ESP32, for which arduino-esp32
  precompiles the much larger Bluedroid (it left 45 KB of RAM with setup
  open, against 106 KB now). An application can't use NimBLE-Arduino's
  `NimBLEDevice` at the same time as pairble.
- Arduino's own one-off reconnect after the first lost connection of a boot is
  used up when pairble starts, so it never runs behind pairble's back.

Generated from the pairble repository by `arduino/build.sh`; report issues
there.
