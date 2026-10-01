/**
 * HTTP transport for protocomm over the device's own access point.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * This is the fallback path, and it only works from the copy of the page
 * served by the device itself: a hosted HTTPS page may not call
 * http://192.168.4.1, and the phone has no route to it anyway while joined to
 * the device's network.
 */

import type { Transport } from "@pairble/core";

export class HttpTransport implements Transport {
  readonly kind = "http" as const;

  constructor(private readonly base = "") {}

  async request(endpoint: string, payload: Uint8Array): Promise<Uint8Array> {
    const response = await fetch(`${this.base}/${endpoint}`, {
      method: "POST",
      // What esp_prov sends. The device is not fussy, but matching Espressif's
      // own client avoids arguing with its HTTP server about content types.
      headers: { "Content-Type": "application/x-www-form-urlencoded" },
      body: payload as BodyInit,
      // protocomm's HTTP transport keeps session state in a cookie. Served
      // from the device, this is same-origin and the browser handles it.
      credentials: "same-origin",
      cache: "no-store",
    });

    if (!response.ok) {
      throw new Error(`${endpoint} returned HTTP ${response.status}`);
    }
    return new Uint8Array(await response.arrayBuffer());
  }

  async close(): Promise<void> {
    // Nothing to tear down; each request stands alone.
  }
}
