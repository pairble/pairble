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

## Install

```sh
idf.py add-dependency "pairble/pairble^0.3.0"     # ESP-IDF, from the ESP Component Registry
```

- **Arduino IDE:** Library Manager → search "pairble".
- **PlatformIO (Arduino):** `lib_deps = pairble/pairble-arduino`, with the
  [pioarduino](https://github.com/pioarduino/platform-espressif32) platform.
- **Web and tooling:** `npm install @pairble/web`, `npx @pairble/cli`.

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

- **Arduino** (arduino-esp32 3.x, Arduino IDE or PlatformIO): the
  [`pairble-arduino`](arduino) library, built from this component by
  `arduino/build.sh`. Add it, call `pairble_start()`; settings that ESP-IDF
  takes from menuconfig are build flags there. In PlatformIO it needs the
  [pioarduino](https://github.com/pioarduino/platform-espressif32) platform,
  as the stock `espressif32` platform still ships arduino-esp32 2.x.
- **PlatformIO with ESP-IDF** (`framework = espidf`):
  [`examples/platformio`](examples/platformio) is a ready project. PlatformIO
  takes the partition table from `board_build.partitions`, not from
  `sdkconfig`, and the default 1 MB app partition is too small.

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
