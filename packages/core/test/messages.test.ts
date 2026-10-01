// SPDX-License-Identifier: Apache-2.0
//
// The codec in src/proto is hand-written, so it is checked against vectors
// produced by Google's protobuf implementation from the same vendored schemas
// (see tools/gen-vectors.py). If these pass, the wire format is right for
// reasons other than the author having read the spec correctly.

import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import test from "node:test";
import * as msg from "../src/proto/messages.js";
import { Writer } from "../src/proto/wire.js";

const vectors: Record<string, string> = JSON.parse(
  readFileSync(new URL("../../test/fixtures/protocomm-vectors.json", import.meta.url), "utf8"),
);

const hex = (b: Uint8Array) => Buffer.from(b).toString("hex");
const bytes = (name: string) => new Uint8Array(Buffer.from(vectors[name]!, "hex"));

test("session commands match the reference encoding", () => {
  assert.equal(hex(msg.sessionCmd0(new Uint8Array(Array.from({ length: 32 }, (_, i) => i)))),
               vectors.sessionCmd0);
  assert.equal(hex(msg.sessionCmd1(new Uint8Array(Array.from({ length: 32 }, (_, i) => 100 + i)))),
               vectors.sessionCmd1);
});

test("scan commands match the reference encoding", () => {
  assert.equal(hex(msg.scanStart()), vectors.scanStart);
  assert.equal(hex(msg.scanStatus()), vectors.scanStatus);
  assert.equal(hex(msg.scanResult(4, 4)), vectors.scanResult_4_4);
});

test("config commands match the reference encoding, including non-ASCII", () => {
  assert.equal(hex(msg.setConfig("Svermigo-ěščř", "p@ss word")), vectors.setConfig_unicode);
  assert.equal(hex(msg.applyConfig()), vectors.applyConfig);
  assert.equal(hex(msg.getStatus()), vectors.getStatus);
});

test("session responses decode", () => {
  const r0 = msg.parseSessionResp0(bytes("sessionResp0"));
  assert.equal(r0.status, msg.Status.Success);
  assert.equal(r0.devicePublicKey.length, 32);
  assert.equal(r0.deviceRandom.length, 16);
  assert.equal(r0.devicePublicKey[0], 32);

  const r1 = msg.parseSessionResp1(bytes("sessionResp1"));
  assert.equal(r1.deviceVerify.length, 32);
  assert.equal(r1.deviceVerify[0], 64);
});

test("scan results decode, negative RSSI and UTF-8 SSIDs included", () => {
  const status = msg.parseScanStatus(bytes("respScanStatus"));
  assert.deepEqual(status, { finished: true, resultCount: 5 });

  const entries = msg.parseScanResult(bytes("respScanResult"));
  assert.equal(entries.length, 3);
  assert.deepEqual(
    entries.map((e) => [e.ssid, e.channel, e.rssi, e.auth]),
    [
      ["Svermigo", 6, -60, msg.AuthMode.WPA2_PSK],
      ["hostna-sit ěščř", 11, -91, msg.AuthMode.WPA_WPA2_PSK],
      ["open-ap", 1, -30, msg.AuthMode.Open],
    ],
  );
  assert.equal(hex(entries[0]!.bssid), "f492bf5d8f70");
});

test("the two failure reasons stay distinguishable", () => {
  // Measured on real firmware: a wrong password and an absent network really
  // do arrive as different reasons, and the product's error message depends
  // on this staying true.
  const auth = msg.parseStatus(bytes("statusAuthError"))!;
  assert.equal(auth.state, msg.StationState.ConnectionFailed);
  assert.equal(auth.failReason, msg.FailReason.AuthError);

  const missing = msg.parseStatus(bytes("statusNetworkNotFound"))!;
  assert.equal(missing.state, msg.StationState.ConnectionFailed);
  assert.equal(missing.failReason, msg.FailReason.NetworkNotFound);
});

test("attempts remaining is readable, which is what a progress bar needs", () => {
  const status = msg.parseStatus(bytes("statusAttemptsRemaining"))!;
  assert.equal(status.state, msg.StationState.Connecting);
  assert.equal(status.attemptsRemaining, 3);
});

test("a successful connection reports its address", () => {
  const status = msg.parseStatus(bytes("statusConnected"))!;
  assert.equal(status.state, msg.StationState.Connected);
  assert.deepEqual(status.connected, {
    ip: "192.168.1.154",
    ssid: "Svermigo",
    channel: 6,
    auth: msg.AuthMode.WPA2_PSK,
  });
});

test("an error reply is not mistaken for a connection", () => {
  // What network_config.c sends when it cannot read the station state:
  // RespGetWifiStatus{ status = InternalError } and nothing else. With the
  // state omitted it would read as Connected, so the status has to survive.
  const resp = new Writer().enum(1, msg.Status.InternalError).finish();
  const status = msg.parseStatus(new Writer().enum(1, 1).message(11, resp).finish());
  assert.equal(status?.status, msg.Status.InternalError);

  assert.equal(msg.parseStatus(new Writer().enum(1, 1).finish()), null, "no resp_get_status at all");
  // An empty resp_get_status is a real reply: Success, Connected.
  assert.deepEqual(msg.parseStatus(new Writer().message(11, new Uint8Array()).finish()), {
    status: msg.Status.Success,
    state: msg.StationState.Connected,
  });
});

test("set and apply replies report their status, and their absence", () => {
  // Success is 0, so a successful reply is an empty message - present, but
  // zero bytes long.
  const ok = (field: number) => new Writer().enum(1, field - 10).message(field, new Uint8Array()).finish();
  const failed = (field: number) =>
    new Writer().enum(1, field - 10).message(field, new Writer().enum(1, msg.Status.InvalidArgument).finish()).finish();

  assert.equal(msg.parseSetConfigStatus(ok(13)), msg.Status.Success);
  assert.equal(msg.parseSetConfigStatus(failed(13)), msg.Status.InvalidArgument);
  assert.equal(msg.parseSetConfigStatus(ok(15)), null, "an apply reply is not a set reply");
  assert.equal(msg.parseApplyConfigStatus(ok(15)), msg.Status.Success);
  assert.equal(msg.parseApplyConfigStatus(failed(15)), msg.Status.InvalidArgument);
  assert.equal(msg.parseApplyConfigStatus(new Uint8Array()), null);
});
