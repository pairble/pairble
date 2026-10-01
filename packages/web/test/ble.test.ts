// SPDX-License-Identifier: Apache-2.0
//
// A connection left open after a failure is not harmless: while a phone holds
// it the device stops advertising, so nobody can find it until the page is
// closed. These drive the transport against a fake Web Bluetooth.

import assert from "node:assert/strict";
import test from "node:test";
import { BleTransport, connectOverBle } from "../src/index.js";

/** Just enough of navigator.bluetooth: one device with a prov-session
 *  endpoint, which can be told to fail at discovery or on the first write. */
function fakeBluetooth(fail: { discovery?: boolean; write?: boolean }) {
  const gatt = {
    connected: false,
    disconnects: 0,
    async connect() {
      gatt.connected = true;
      return server;
    },
    disconnect() {
      gatt.connected = false;
      gatt.disconnects++;
    },
  };
  const characteristic = {
    async getDescriptor() {
      return { readValue: async () => new DataView(new TextEncoder().encode("prov-session").buffer) };
    },
    async writeValueWithResponse() {
      if (fail.write) throw new Error("GATT operation failed for unknown reason.");
    },
    async readValue() {
      return new DataView(new ArrayBuffer(0));
    },
  };
  const server = {
    disconnect: () => gatt.disconnect(),
    async getPrimaryService() {
      if (fail.discovery) throw new Error("GATT Server is disconnected. Cannot retrieve services.");
      return { getCharacteristics: async () => [characteristic] };
    },
  };
  const device = { gatt, addEventListener() {}, removeEventListener() {} };
  Object.defineProperty(globalThis, "navigator", {
    configurable: true,
    value: { bluetooth: { requestDevice: async () => device } },
  });
  return gatt;
}

test("a failed service discovery lets go of the device", async () => {
  const gatt = fakeBluetooth({ discovery: true });
  await assert.rejects(() => BleTransport.connect(), /GATT Server is disconnected/);
  assert.equal(gatt.connected, false);
  assert.equal(gatt.disconnects, 1);
});

test("a failed handshake lets go of the device", async () => {
  const gatt = fakeBluetooth({ write: true });
  await assert.rejects(() => connectOverBle({ pop: "K3F9Q2M7ZX" }), /GATT operation failed/);
  assert.equal(gatt.connected, false);
  assert.equal(gatt.disconnects, 1);
});

