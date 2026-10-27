# @pairble/core

The protocol client behind [pairble](https://pairble.com): Espressif protocomm
with security 1 (X25519, the device's proof of possession, AES-CTR), Wi-Fi
scan, credentials and status. Hand-written protobuf on WebCrypto - no DOM, no
Node APIs, no dependencies. Bring a transport; `@pairble/web` has Web
Bluetooth and HTTP ones.

```ts
import { ProvisioningSession, ProvisioningError } from "@pairble/core";

const session = new ProvisioningSession(transport, { pop: "K3F9Q2M7ZX4P" });
await session.establish();
const networks = await session.scan();
await session.applyCredentials("Home", "password");
try {
  await session.waitForResult((p) => console.log(p.phase));
} catch (e) {
  if (e instanceof ProvisioningError) console.log(e.reason); // wrong-password, network-not-found, ...
}
```

Firmware, docs and the rest: [github.com/pairble/pairble](https://github.com/pairble/pairble).
Apache-2.0.
