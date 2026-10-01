# pairble in PlatformIO

An ESP-IDF project (`framework = espidf`) on the
[pioarduino](https://github.com/pioarduino/platform-espressif32) platform,
which follows ESP-IDF releases; pairble needs ESP-IDF 5.1 or newer.

```sh
pio run -e esp32-s3 -t upload -t monitor     # or -e esp32, -e esp32-c3
```

With no network stored, the board opens setup and logs the URL for the QR
code:

```
I (825) example: setup open - scan the QR code: https://pairble.com/s#name=PAIRBLE-BA6550&pop=K3F9Q2M7ZX4P
```

Once a phone has set a network, it connects on every start:

```
I (585) pairble: credentials stored, connecting
I (4135) example: online
```

To open setup again, switch the board on three times in a row, each time for
less than 5 seconds (or press its reset button three times, a second or two
apart). Nothing is erased: it keeps the old network until a new one works.

In your own project:

- `src/idf_component.yml`: depend on `pairble/pairble` from the ESP Component
  Registry (this example points at the copy in this repository).
- `sdkconfig.defaults`: Bluetooth on, NimBLE host.
- `platformio.ini`: `board_build.partitions = partitions_singleapp_large.csv`
  or a table of your own with an app partition of at least 1.5 MB. PlatformIO
  ignores the partition table set in `sdkconfig`, and Wi-Fi + BLE +
  provisioning does not fit the default 1 MB.

Arduino (`framework = arduino, espidf`) needs a few more steps; see the main
[README](../../README.md#arduino-and-platformio).
