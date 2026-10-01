/**
 * @pairble/core - transport-agnostic ESP32 provisioning client.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Deliberately free of DOM references. The same client runs under Web
 * Bluetooth in a browser, over HTTP against the device's own access point, and
 * in Node against real firmware - and that last one is the point, because a
 * hand-written Security1 implementation that is never checked against the C
 * side is a liability.
 */

export { ProvisioningSession, ProvisioningError } from "./session.js";
export type { SessionOptions, Progress, ScanEntry, DeviceStatus } from "./session.js";
export { AuthMode, FailReason, StationState } from "./session.js";
export type { Transport } from "./transport.js";
export { SCAN_BATCH_BLE, SCAN_BATCH_HTTP } from "./transport.js";
