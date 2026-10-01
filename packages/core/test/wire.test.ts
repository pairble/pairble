// SPDX-License-Identifier: Apache-2.0
import assert from "node:assert/strict";
import test from "node:test";
import { asInt32, asUint32, readFields, Writer } from "../src/proto/wire.js";

test("varint round trip across byte boundaries", () => {
  for (const value of [0, 1, 127, 128, 300, 16383, 16384, 0xffffffff]) {
    const buf = new Writer().uint32(1, value).finish();
    let seen = -1;
    readFields(buf, (f) => {
      if (f.no === 1) seen = asUint32(f.varint);
    });
    // Zero is the proto3 default and is not written at all, so it decodes as
    // "field absent" rather than as a value - which is correct, not a bug.
    assert.equal(seen, value === 0 ? -1 : value);
  }
});

test("negative int32 survives the ten-byte encoding", () => {
  // Every RSSI is negative, so this path runs on every scan result.
  for (const rssi of [-1, -30, -60, -91, -128]) {
    const buf = new Writer().uint32(3, rssi).finish();
    let seen = 0;
    readFields(buf, (f) => {
      if (f.no === 3) seen = asInt32(f.varint);
    });
    assert.equal(seen, rssi);
    assert.equal(buf.length, 11, "negative varints occupy ten bytes plus a tag");
  }
});

test("unknown fields are skipped, so newer firmware does not break the client", () => {
  const buf = new Writer().uint32(1, 7).string(99, "from the future").uint32(2, 9).finish();
  const seen: number[] = [];
  readFields(buf, (f) => {
    if (f.type === 0) seen.push(asUint32(f.varint));
  });
  assert.deepEqual(seen, [7, 9]);
});

test("truncated input is rejected rather than silently misread", () => {
  assert.throws(() => readFields(new Uint8Array([0x0a, 0x05, 0x01]), () => {}), /truncated/);
});
