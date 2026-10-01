# pairble

**Scan the code, join the network.** Wi-Fi onboarding for ESP32 over Bluetooth, with nothing to install.

The user scans the QR code on the box with the phone's camera. On iPhone the
pairble App Clip opens, on Android and computers a web page that talks
Bluetooth. They see the networks **the device itself can reach**, pick one,
type the password, and the device is online - usually within half a minute.
Credentials travel over an encrypted protocomm session (security 1, a per-unit
proof of possession from the QR code) and are never logged.

```c
#include "esp_log.h"
#include "pairble.h"

void app_main(void)
{
    /* Connects if a network is stored, otherwise opens setup. */
    ESP_ERROR_CHECK(pairble_start(NULL));

    /* This address goes into the QR code on the box. */
    ESP_LOGI("example", "QR code: %s", pairble_setup_url());
}
```

- **The whole integration is one call.** NVS, networking and Wi-Fi are set up
  if the application has not, and left alone if it has.
- **A plan B:** at the same time as Bluetooth the device opens its own WPA2
  network with the setup page in flash (`pairble_wifi_qr()` is its QR code).
- **Survives a new router:** when the stored network stops answering, setup
  reopens by itself without erasing anything; a button
  (`CONFIG_PAIRBLE_SETUP_BUTTON_GPIO`), quick power cycles or
  `pairble_open_setup()` open it on request.
- **Clear errors:** wrong password and network not found are told apart, and
  the person can retry without starting over.
- **Other ways in:** `pairble_set_network()` takes a network from a serial
  protocol such as Improv, a keypad or a menu, with the same safety: the old
  network stays until the new one works.
- **pairble.com is free to use, in production too**, and is the default setup
  address. See Kconfig for everything else.

Bluetooth LE is required: ESP32, ESP32-C2, C3, C5, C6 and S3. ESP-IDF 5.1 or
later, 5.5 for the ESP32-C5 (tested with 5.4 and 5.5).

Documentation, code samples and guides: [pairble.com](https://pairble.com).
Questions: support@pairble.com. Licence: Apache-2.0.
