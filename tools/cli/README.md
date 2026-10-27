# @pairble/cli

Production-line tooling for [pairble](https://pairble.com) devices: a
per-unit proof of possession, the device name, the setup URL and its QR code,
and an NVS image that puts the PoP on the chip.

```sh
npx @pairble/cli unit --mac 24:58:7c:ba:65:50 --out unit-ba6550
```

```
pairble unit --mac <mac> [--pop <pop>] [--base <url>] [--prefix PAIRBLE-] [--out <dir>]
pairble pop                       print a new proof of possession
pairble name <mac> [--prefix P]   device name the firmware will advertise
pairble url --mac <mac> --pop <pop> [--base <url>] [--prefix P]
pairble qr  --mac <mac> --pop <pop> [--base <url>] [--prefix P] [--svg <file>]
```

The MAC is the base MAC printed by `esptool.py --port <port> read_mac`.
Firmware and docs: [github.com/pairble/pairble](https://github.com/pairble/pairble).
Apache-2.0.
