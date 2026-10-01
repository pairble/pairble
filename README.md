# pairble

> **Scan the code, join the network.** Wi-Fi onboarding for ESP32 over Bluetooth, with nothing to install.

The user scans a QR code on the box. They see the Wi-Fi networks **the device itself can see**, pick one, type the password, and the device is online. On Android that screen is a web page using Web Bluetooth; on iPhone it is an App Clip. No app store visit on either.

**Status:** run end to end on real ESP32-S3 hardware with an iPhone (App Clip and full app), Android Chrome, desktop Chrome and the device's own Wi-Fi network. Web page and documentation: [pairble.com](https://pairble.com).

## Why

- **"Install our app"** is the biggest drop-off in any hardware unboxing.
- **SoftAP captive portals** make the user leave their network, often fail to pop up, and trigger "no internet" warnings.
- **Typing the SSID** lets people pick their 5 GHz network and file a ticket.

pairble asks the *device* which networks it can see. Most ESP32 radios are 2.4 GHz only, so the list is right by construction (the ESP32-C5 also does 5 GHz).

## How it works

| Phone | What the user does | Transport |
|---|---|---|
| **Android** | Camera → QR → Chrome opens the page → Connect | Web Bluetooth |
| **iPhone** | Camera → QR → App Clip card → Set up | CoreBluetooth |

Both speak Espressif's [protocomm](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/provisioning/protocomm.html) with security 1: X25519 key agreement mixed with a per-device proof of possession from the QR code, then AES-CTR. Credentials never travel in the clear, and the firmware never logs them.

## Packages

| Path | What it is |
|---|---|
| [`components/pairble`](components/pairble) | ESP-IDF component on `espressif/network_provisioning`. `pairble_start()` is the whole integration. |
| [`packages/core`](packages/core) | `@pairble/core` — protocol client in TypeScript: hand-written protobuf, security 1 on WebCrypto, scan / config / status. No DOM, no Node APIs. |
| [`packages/web`](packages/web) | `@pairble/web` — Web Bluetooth and HTTP transports, plus the setup page: one self-contained file, 26 KB gzipped, in ten languages. |
| [`tools/cli`](tools/cli) | `@pairble/cli` — production line: per-unit PoP, device name, QR code, NVS image. |

The iPhone side is the pairble App Clip, run by pairble at
[pairble.com](https://pairble.com): every device whose QR code points there -
the default - works with it, in production too and at no cost. An App Clip of
your own on your own domain is possible; it needs Apple's approval, ideally as
part of your own app. Write to support@pairble.com.

## Quick start (firmware)

```c
void app_main(void)
{
    ESP_ERROR_CHECK(pairble_start(NULL));
    ESP_LOGI(TAG, "QR code: %s", pairble_setup_url());
}
```

That is the whole integration. `pairble_start()` connects with stored
credentials, or opens setup over BLE; it sets up NVS, networking and Wi-Fi if
the application has not, and leaves them alone if it has.
`pairble_setup_url()` is what goes into the QR code:
`https://pairble.com/s#name=PAIRBLE-BA6550&pop=K3F9Q2M7ZX4P`.

Everything is optional:

```c
pairble_config_t config = PAIRBLE_CONFIG_DEFAULT();
config.name_prefix = "LAMP-";   // advertises LAMP-BA6550 (default PAIRBLE-)
config.name        = NULL;      // or an exact name, for one-off devices
config.pop         = NULL;      // NULL: from NVS, else generated on first boot
config.event_cb    = on_event;  // started, failed (and why), online, ...
ESP_ERROR_CHECK(pairble_start(&config));
```

A second, smaller QR code - `pairble_wifi_qr()`, e.g.
`WIFI:S:PAIRBLE-BA6550;T:WPA;P:K3F9Q2M7ZX4P;;` - joins the device's own Wi-Fi,
which runs alongside BLE during setup, for phones that cannot use Bluetooth: an
iPhone with no internet to fetch the App Clip, a browser without Web Bluetooth,
a laptop. The device serves the same setup page there, and most phones open it
on their own. It is WPA2 with the proof of possession as its password. Turn it
off with `config.softap = false` or in menuconfig.

The proof of possession - the secret in the QR code - comes from NVS if
[`@pairble/cli`](tools/cli) wrote one at manufacturing, which is what a
printed label needs. Otherwise the device generates one on first boot and keeps
it, which suits devices that show their QR code on a display.

See [`examples/basic`](examples/basic),
[`examples/existing_app`](examples/existing_app) and, for PlatformIO,
[`examples/platformio`](examples/platformio).

## Arduino and PlatformIO

pairble is an ESP-IDF component; on PlatformIO it works in an ESP-IDF
project (`framework = espidf`) and in Arduino as an ESP-IDF component
(`framework = arduino, espidf`, the [pioarduino](https://github.com/pioarduino/platform-espressif32)
platform, ESP-IDF 5). [`examples/platformio`](examples/platformio) is a
ready ESP-IDF project; note that PlatformIO takes the partition table from
`board_build.partitions`, not from `sdkconfig`, and the default 1 MB app
partition is too small. The plain Arduino framework cannot build it: its
ESP-IDF comes precompiled, without menuconfig. With Arduino, four things are
needed - all found building a real Arduino firmware on it:

1. **Tell Arduino that BLE is in use**, or it releases the Bluetooth memory
   at startup and the controller crashes on its first start ("HLI Magic
   mismatch"): `extern "C" bool bleInUse(void) { return true; }`
2. **Override `espressif/network_provisioning`**: Arduino 3.x pins 1.0.2 on
   ESP-IDF 5; pairble needs 1.2.5, which also fixes malformed-message bugs
   reachable over Bluetooth. In the project's `idf_component.yml`, give the
   dependency an `override_path` to a copy of 1.2.5.
3. In `sdkconfig.defaults`: `CONFIG_BT_ENABLED=y`, `CONFIG_BT_NIMBLE_ENABLED=y`,
   and what Arduino itself expects - `CONFIG_COMPILER_CXX_EXCEPTIONS=y`,
   `CONFIG_COMPILER_CXX_RTTI=y`, `CONFIG_MBEDTLS_PSK_MODES=y` and
   `CONFIG_MBEDTLS_KEY_EXCHANGE_PSK=y` (without them `NetworkClientSecure`
   does not link).
4. Call `pairble_start()` before anything touches Arduino's `WiFi` object,
   and switch Wi-Fi off with `esp_wifi_stop()` rather than `WiFi.disconnect()`.

## Supported targets

Bluetooth LE is mandatory: targets without it fail at compile time.

| Target | Notes |
|---|---|
| ESP32-S3, C3, C6 | recommended |
| ESP32 | watch the NimBLE-during-Wi-Fi-scan regression on IDF 5.5.3 |
| ESP32-C2 (ESP8684) | BLE 5.3; tight on RAM |
| ESP32-C5 | also 5 GHz, so the scan can list networks the product may not want |
| ESP32-S2 | **not supported** — no Bluetooth |

Requires ESP-IDF 5.1 or later (5.5 for the ESP32-C5); tested with 5.4 and 5.5.

## License

Apache-2.0, see [LICENSE](LICENSE). Copyright 2026 Pajenicko.
