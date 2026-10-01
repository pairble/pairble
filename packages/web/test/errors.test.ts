// SPDX-License-Identifier: Apache-2.0
import assert from "node:assert/strict";
import test from "node:test";
import { ProvisioningError } from "@pairble/core";
import { classifyError } from "../app/errors.js";

const dom = (name: string, message: string) => Object.assign(new Error(message), { name });
const prov = (reason: string) => Object.assign(new Error(reason), { name: "ProvisioningError", reason });

test("a dismissed chooser is calm, not an error", () => {
  assert.equal(classifyError(dom("NotFoundError", "User cancelled the requestDevice() chooser.")), "cancelled");
});

test("Bluetooth switched off", () => {
  assert.equal(classifyError(dom("NotFoundError", "Bluetooth adapter not available.")), "bluetooth-off");
});

test("no matching device", () => {
  assert.equal(classifyError(dom("NotFoundError", "No Services matching UUID found in Device.")), "no-device");
  assert.equal(classifyError(new Error("no prov-session endpoint on 021a... - wrong service UUID")), "no-device");
});

test("a wrong proof of possession gets its own verdict", () => {
  assert.equal(classifyError(new ProvisioningError("code mismatch", "wrong-pop")), "wrong-code");
  // The old message-matching path is gone on purpose: a reworded message must
  // not silently turn a wrong code into "unknown".
  assert.equal(classifyError(new Error("sec1: device verification failed")), "unknown");
});

test("the device's own verdicts", () => {
  assert.equal(classifyError(prov("wrong-password")), "wrong-password");
  assert.equal(classifyError(prov("network-not-found")), "network-not-found");
  assert.equal(classifyError(prov("timeout")), "timeout");
});

test("a dropped link", () => {
  assert.equal(classifyError(dom("NetworkError", "GATT Server is disconnected. Cannot perform GATT operations.")), "lost");
  assert.equal(classifyError(new TypeError("Failed to fetch")), "lost");
});

test("anything else is unknown rather than guessed", () => {
  assert.equal(classifyError(new Error("something odd")), "unknown");
  assert.equal(classifyError(null), "unknown");
  assert.equal(classifyError("a string"), "unknown");
});
