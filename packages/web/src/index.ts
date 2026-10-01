/**
 * @pairble/web - browser transports for @pairble/core.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * One source, two deployments (design doc section 4.5):
 *
 *   hosted    an HTTPS page on the vendor's domain. A secure context, so Web
 *             Bluetooth is available. This is the Android path from the QR code.
 *
 *   embedded  the same file, built as one self-contained HTML document, flashed
 *             into the device and served from http://192.168.4.1. Not a secure
 *             context, so navigator.bluetooth is absent by definition - and not
 *             needed, because the phone is already on the device's network.
 *
 * Which one applies is decided by where the page finds itself running, not by
 * a build flag, so the same artefact works in both places.
 */

import { ProvisioningSession, type SessionOptions } from "@pairble/core";
import { BleTransport, type BleConnectOptions } from "./ble.js";
import { HttpTransport } from "./http.js";

export { BleTransport, ESP_PROV_EXAMPLE_SERVICE } from "./ble.js";
export type { BleConnectOptions } from "./ble.js";
export { HttpTransport } from "./http.js";

export type TransportChoice = "ble" | "http" | "unsupported";

/**
 * Web Bluetooth is gated on secure contexts: HTTPS and http://localhost only.
 * On file://, plain http:// and private addresses it is simply not there.
 * Android WebView does not implement it at all, which is how most in-app
 * browsers behave - so this returning "unsupported" is a routine case to
 * handle in the UI, not an error.
 */
export function detectTransport(): TransportChoice {
  const hasBluetooth =
    typeof navigator !== "undefined" &&
    "bluetooth" in navigator &&
    typeof isSecureContext !== "undefined" &&
    isSecureContext;
  if (hasBluetooth) return "ble";
  if (typeof location !== "undefined" && location.protocol === "http:") return "http";
  return "unsupported";
}

/** Opens a session over BLE. Call this from a click handler: the chooser
 *  requires a user gesture. */
export async function connectOverBle(
  options: SessionOptions & BleConnectOptions,
): Promise<ProvisioningSession> {
  const transport = await BleTransport.connect(options);
  const session = new ProvisioningSession(transport, options);
  try {
    await session.establish();
  } catch (error) {
    // A failed handshake still holds the link, and a held link keeps the
    // device from advertising - release it before reporting.
    await transport.close().catch(() => {});
    throw error;
  }
  return session;
}

/** Opens a session against the device serving this page, over protocomm on
 *  HTTP - the stock ESP-IDF SoftAP provisioning transport (/prov-session and
 *  so on). The pairble firmware's own SoftAP serves a simpler API instead,
 *  which the setup page talks to directly (app/local.ts). */
export async function connectOverHttp(options: SessionOptions & { base?: string }): Promise<ProvisioningSession> {
  const session = new ProvisioningSession(new HttpTransport(options.base ?? ""), options);
  await session.establish();
  return session;
}
