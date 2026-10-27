# @pairble/web

Web Bluetooth and HTTP transports for [`@pairble/core`](https://www.npmjs.com/package/@pairble/core),
plus helpers to pick between them. The setup page at
[pairble.com/s](https://pairble.com/s) is built on it.

```ts
import { connectOverBle, detectTransport } from "@pairble/web";

if (detectTransport() === "ble") {
  // From a click handler: the Bluetooth chooser needs a user gesture.
  const session = await connectOverBle({ pop: "K3F9Q2M7ZX4P", namePrefix: "PAIRBLE-" });
  const networks = await session.scan();
  await session.applyCredentials(networks[0].ssid, "password");
  await session.waitForResult();
}
```

Web Bluetooth needs Chrome or Edge on Android, Windows, macOS or ChromeOS, and
a secure context. Firmware, docs and the rest:
[github.com/pairble/pairble](https://github.com/pairble/pairble). Apache-2.0.
