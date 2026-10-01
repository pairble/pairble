/**
 * ?demo=1 - a simulated device, so the interface can be reviewed without
 * hardware. Timings are the ones measured against a real ESP32-S3 with the
 * pairble firmware (two connection attempts), so the waits feel as long as
 * they really are.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Triggers: a password starting with "wrong" (e.g. "wrongpass" - it has to
 * pass the 8-character check first) fails as a wrong password; the weak
 * "O2-Internet-42", or "missing" typed under Other network, fails as not
 * found. Anything else joins. Add &fast=1 to run five times faster.
 */

import type { Client, Connect, JoinProgress, ScanResult } from "./client.js";
import type { Env } from "./env.js";

/** &env=ios | android | webview | firefox | desktop | insecure | http shows
 *  that environment's screen instead of the BLE flow. */
export function demoEnv(name: string | null): Env {
  switch (name) {
    case "ios": return { kind: "ios" };
    case "android": return { kind: "android", browser: "other" };
    case "webview": return { kind: "android", browser: "webview" };
    case "firefox": return { kind: "android", browser: "firefox" };
    case "desktop": return { kind: "desktop" };
    case "insecure": return { kind: "insecure" };
    case "http": return { kind: "http" };
    default: return { kind: "ble" };
  }
}

class DemoError extends Error {
  constructor(message: string, readonly reason: string) {
    super(message);
    this.name = "ProvisioningError";
  }
}

/** The networks in the page's language - a few are names people choose. */
export interface DemoNames {
  living: string; // with the language's own accented letters: SSIDs are UTF-8
  cafe: string;
  neighbour: string;
}

function networks(names: DemoNames): ScanResult[] {
  return [
    { ssid: "Svermigo", rssi: -58, auth: 3 },
    { ssid: "Svermigo", rssi: -79, auth: 3 },          // the repeater: collapses into the row above
    { ssid: names.living, rssi: -66, auth: 4 },
    { ssid: "", rssi: -63, auth: 3 },                 // hidden: dropped
    { ssid: "FRITZ!Box 7530 XY", rssi: -71, auth: 7 },
    { ssid: names.cafe, rssi: -74, auth: 0 },
    { ssid: "Pajenicko-802.1X", rssi: -77, auth: 5 },     // enterprise: shown, not selectable
    { ssid: "Internet-42", rssi: -84, auth: 3 },
    { ssid: "A network name that goes on for rather longer than it should", rssi: -88, auth: 3 },
    { ssid: names.neighbour, rssi: -91, auth: 0 },
  ];
}

export function demoConnect(fast: boolean, names: DemoNames): Connect {
  const scale = fast ? 0.2 : 1;
  const wait = (ms: number) => new Promise<void>((resolve) => setTimeout(resolve, ms * scale));

  return async () => {
    await wait(2200); // chooser plus handshake, as measured
    let ssid = "";
    let password = "";
    const client: Client = {
      async scan() {
        await wait(5200);
        return networks(names).map((n) => ({ ...n }));
      },
      async applyCredentials(s, p) {
        ssid = s;
        password = p;
        await wait(400);
      },
      async waitForResult(onProgress: (p: JoinProgress) => void) {
        const wrongPassword = password.startsWith("wrong");
        const notFound = ssid === "missing" || ssid === "O2-Internet-42";
        const fails = wrongPassword || notFound;
        // The first attempt reports no count; on real firmware nothing has
        // failed yet, so there is nothing to count.
        for (let second = 0; second < (fails ? 21 : 8); second++) {
          onProgress(fails && second >= 11 ? { attemptsRemaining: 1 } : {});
          await wait(1000);
        }
        if (notFound) throw new DemoError("network not found", "network-not-found");
        if (wrongPassword) throw new DemoError("wrong password", "wrong-password");
        return { connected: { ip: "192.168.1.154" } };
      },
      async close() {},
    };
    return client;
  };
}
