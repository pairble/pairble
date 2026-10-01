// SPDX-License-Identifier: Apache-2.0
//
// The cipher is checked against OpenSSL through node:crypto, an implementation
// that shares no code with the WebCrypto path under test. What matters most
// here is the continuity: the device keeps one AES-CTR keystream for the whole
// session, so a payload of unaligned length leaves the next message starting
// mid-block. Encrypting each message independently would look correct in
// isolation and fail against firmware on the third exchange.

import assert from "node:assert/strict";
import { createCipheriv, randomBytes } from "node:crypto";
import test from "node:test";
import { Sec1Cipher } from "../src/crypto/security1.js";

function openssl(key: Buffer, nonce: Buffer, chunks: Uint8Array[]): Uint8Array[] {
  const cipher = createCipheriv("aes-256-ctr", key, nonce);
  return chunks.map((c) => new Uint8Array(cipher.update(Buffer.from(c))));
}

test("keystream continues across unaligned message boundaries", async () => {
  const key = randomBytes(32);
  const nonce = randomBytes(16);

  // 32 and 32 are the handshake verifiers; the rest are deliberately not
  // multiples of the block size, as real payloads are not.
  const chunks = [32, 32, 7, 1, 61, 16, 3].map((n) => new Uint8Array(randomBytes(n)));

  const expected = openssl(key, nonce, chunks);
  const cipher = await Sec1Cipher.create(new Uint8Array(key), new Uint8Array(nonce));

  for (let i = 0; i < chunks.length; i++) {
    const actual = await cipher.xor(chunks[i]!);
    assert.deepEqual(Buffer.from(actual), Buffer.from(expected[i]!), `chunk ${i} (${chunks[i]!.length} bytes)`);
  }
});

test("the counter carries past a block boundary", async () => {
  // A nonce ending in 0xff..ff forces the carry that a naive last-byte
  // increment would get wrong.
  const key = randomBytes(32);
  const nonce = Buffer.concat([randomBytes(14), Buffer.from([0xff, 0xfe])]);
  const data = new Uint8Array(randomBytes(64));

  const [expected] = openssl(key, nonce, [data]);
  const cipher = await Sec1Cipher.create(new Uint8Array(key), new Uint8Array(nonce));
  assert.deepEqual(Buffer.from(await cipher.xor(data)), Buffer.from(expected!));
});

test("CTR is its own inverse, so one side's encrypt is the other's decrypt", async () => {
  const key = new Uint8Array(randomBytes(32));
  const nonce = new Uint8Array(randomBytes(16));
  const plaintext = new Uint8Array(randomBytes(45));

  const sender = await Sec1Cipher.create(key, nonce);
  const receiver = await Sec1Cipher.create(key, nonce);
  const ciphertext = await sender.xor(plaintext);
  assert.deepEqual(Buffer.from(await receiver.xor(ciphertext)), Buffer.from(plaintext));
});

test("a device random of the wrong length is refused", async () => {
  await assert.rejects(
    () => Sec1Cipher.create(new Uint8Array(32), new Uint8Array(8)),
    /16 bytes/,
  );
});
