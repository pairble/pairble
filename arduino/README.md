# pairble for Arduino

Wi-Fi setup for ESP32 over Bluetooth, with nothing to install: the phone's
camera opens the pairble App Clip (iPhone) or a web page (Android, computers
with Bluetooth), the device shows the networks it can see, and the password
travels over an encrypted Bluetooth session. Phones that cannot use Bluetooth
join the device's own Wi-Fi instead. See [pairble.com](https://pairble.com).

This is the ESP-IDF component [pairble](https://github.com/pairble/pairble),
packaged for Arduino. It needs **arduino-esp32 3.x** (ESP-IDF 5); in
PlatformIO that is the [pioarduino](https://github.com/pioarduino/platform-espressif32)
platform - the stock `espressif32` platform still ships arduino-esp32 2.x.

## Use

```cpp
#include <pairble.h>

void setup()
{
  Serial.begin(115200);
  ESP_ERROR_CHECK(pairble_start(NULL));
  Serial.printf("QR code: %s\n", pairble_setup_url());
}
```

With a network stored the board connects; without one it opens setup and
`pairble_setup_url()` is what goes into the QR code. Arduino's `WiFi` object
knows about the connection - `WiFi.status()`, `WiFi.localIP()`, `WiFi.SSID()`,
`WiFi.RSSI()` and `WiFi.scanNetworks()` work, and so do `HTTPClient` and
`WiFiClient`. Just don't connect with `WiFi.begin()`: pairble owns the
station. A network from elsewhere - Improv over serial, a keypad - goes
through `pairble_set_network()`, which keeps the old one until the new one
works. To switch Wi-Fi off before deep sleep, call
`esp_wifi_stop()` rather than `WiFi.disconnect()`, so it does not count as the
network being gone.

```ini
; platformio.ini
[env:esp32-s3]
platform = https://github.com/pioarduino/platform-espressif32/releases/download/stable/platform-espressif32.zip
framework = arduino
board = esp32-s3-devkitc-1
board_build.partitions = huge_app.csv   ; the default 1.25 MB app partition is too small
lib_deps = pairble/pairble-arduino
```

Wi-Fi, Bluetooth and setup together take about 1.2 MB of flash on the
ESP32-S3, 1.3 MB on the ESP32-C3 and 1.7 MB on the original ESP32 (whose
Bluetooth host is bigger): more than the default 1.25 MB app partition, so
pick a larger one (Arduino IDE: Tools > Partition Scheme > Huge APP).

## Settings

The ESP-IDF version is configured in menuconfig; here every setting has the
same default and can be overridden with a build flag - see
`src/pairble_arduino_config.h`:

```ini
build_flags =
    -D CONFIG_PAIRBLE_NAME_PREFIX='"LAMP-"'
    -D CONFIG_PAIRBLE_SETUP_BUTTON_GPIO=0
```

Most can also be set at run time in `pairble_config_t`; see `pairble.h`.

## Notes

- arduino-esp32 precompiles `espressif/network_provisioning` 1.0.2. This
  library brings its own newer copy (Apache-2.0, `LICENSE.network_provisioning`),
  which replaces it at link time.
- On the original ESP32, Arduino uses the Bluedroid Bluetooth host; dropping
  an idle Bluetooth client after `CONFIG_PAIRBLE_BLE_IDLE_TIMEOUT_SECONDS` works
  with NimBLE only (ESP32-S3, -C3, -C6...).

Generated from the pairble repository by `arduino/build.sh`; report issues
there.
