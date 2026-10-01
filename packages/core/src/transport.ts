/**
 * SPDX-License-Identifier: Apache-2.0
 *
 * A transport carries opaque protocomm payloads to one named endpoint and
 * brings the reply back. Everything above this interface is identical whether
 * the bytes travel over a GATT characteristic or an HTTP POST, which is what
 * lets the BLE page and the copy served from the device's own flash share a
 * client rather than being two implementations.
 */
export interface Transport {
  readonly kind: "ble" | "http";
  /** `prov-session`, `prov-scan`, `prov-config`, `proto-ver`. */
  request(endpoint: string, payload: Uint8Array): Promise<Uint8Array>;
  close(): Promise<void>;
}

/** How many scan entries to pull per round trip. Espressif's own tooling drops
 *  to 4 over BLE because the responses otherwise outgrow the MTU. */
export const SCAN_BATCH_BLE = 4;
export const SCAN_BATCH_HTTP = 100;
