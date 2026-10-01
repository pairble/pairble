// SPDX-License-Identifier: Apache-2.0
//
// The handshake end to end, against a device emulated in this file. The device
// side is written from security1.c with node:crypto - OpenSSL, sharing no code
// with the WebCrypto client - so a mistake in the client cannot be mirrored by
// the same mistake in the test.

import assert from "node:assert/strict";
import { createCipheriv, createHash, diffieHellman, generateKeyPairSync, randomBytes, createPublicKey } from "node:crypto";
import test from "node:test";
import { ProvisioningError, ProvisioningSession, type Transport } from "../src/index.js";
import { Status } from "../src/proto/messages.js";
import { readFields, Writer } from "../src/proto/wire.js";

/** Raw 32-byte X25519 public key <-> Node KeyObject. */
const X25519_SPKI_PREFIX = Buffer.from("302a300506032b656e032100", "hex");
const toRaw = (key: ReturnType<typeof createPublicKey>) => key.export({ type: "spki", format: "der" }).subarray(12);
const fromRaw = (raw: Uint8Array) =>
  createPublicKey({ key: Buffer.concat([X25519_SPKI_PREFIX, raw]), format: "der", type: "spki" });

/** Just enough of protocomm's prov-session endpoint to run security 1. */
class EmulatedDevice implements Transport {
  readonly kind = "ble" as const;
  private readonly keys = generateKeyPairSync("x25519");
  private readonly random = randomBytes(16);
  private cipher?: ReturnType<typeof createCipheriv>;
  private clientPub?: Buffer;
  private linkUp = true;
  /** Drop the link instead of answering command 1, as the firmware does on a wrong PoP. */
  dropOnCommand1 = false;
  /** Answers prov-config: plaintext in, plaintext out; encryption is done here. */
  config?: (request: Uint8Array) => Uint8Array;

  constructor(private readonly pop: string) {}

  async request(endpoint: string, payload: Uint8Array): Promise<Uint8Array> {
    if (endpoint === "prov-config") {
      assert.ok(this.cipher && this.config, "prov-config before the session");
      return this.cipher.update(this.config(this.cipher.update(payload)));
    }
    assert.equal(endpoint, "prov-session");
    let sec1 = new Uint8Array();
    readFields(payload, (f) => { if (f.no === 11) sec1 = f.bytes.slice(); });

    let cmd0: Uint8Array | undefined;
    let cmd1: Uint8Array | undefined;
    readFields(sec1, (f) => {
      if (f.no === 20) readFields(f.bytes, (g) => { if (g.no === 1) cmd0 = g.bytes.slice(); });
      if (f.no === 22) readFields(f.bytes, (g) => { if (g.no === 2) cmd1 = g.bytes.slice(); });
    });

    if (cmd0) {
      this.clientPub = Buffer.from(cmd0);
      const shared = diffieHellman({ privateKey: this.keys.privateKey, publicKey: fromRaw(cmd0) });
      const popHash = createHash("sha256").update(this.pop).digest();
      const symKey = Buffer.from(shared.map((b, i) => b ^ popHash[i]!));
      // One cipher object for the whole session: the keystream is continuous,
      // exactly as mbedtls keeps nc_off in the firmware.
      this.cipher = createCipheriv("aes-256-ctr", symKey, this.random);
      const resp0 = new Writer().bytes(2, toRaw(this.keys.publicKey)).bytes(3, this.random).finish();
      const out = new Writer().enum(1, 1).message(21, resp0).finish();
      return new Writer().enum(2, 1).message(11, out).finish();
    }

    assert.ok(cmd1 && this.cipher && this.clientPub, "command1 before command0");
    if (this.dropOnCommand1) {
      this.linkUp = false;
      throw new Error("GATT Server is disconnected. Cannot perform GATT operations.");
    }
    const decrypted = this.cipher.update(cmd1);
    if (!decrypted.equals(toRaw(this.keys.publicKey))) {
      // What the firmware does: security1.c returns ESP_FAIL and the BLE
      // transport answers the write with BLE_ATT_ERR_INVALID_PDU, so the
      // client's request fails rather than receiving a response.
      throw new Error("device: client verifier mismatch");
    }
    const deviceVerify = this.cipher.update(this.clientPub);
    const resp1 = new Writer().bytes(3, deviceVerify).finish();
    const out = new Writer().enum(1, 3).message(23, resp1).finish();
    return new Writer().enum(2, 1).message(11, out).finish();
  }

  async close(): Promise<void> {}
}

test("security 1 handshake completes against an independently written device", async () => {
  const session = new ProvisioningSession(new EmulatedDevice("K3F9Q2M7ZX"), { pop: "K3F9Q2M7ZX" });
  await session.establish();
});

test("a wrong proof of possession is reported as such, not as a generic failure", async () => {
  const session = new ProvisioningSession(new EmulatedDevice("K3F9Q2M7ZX"), { pop: "abcd1234" });
  // The emulated device rejects command 1 the way the firmware does - by
  // failing the request - and the session must translate that, specifically.
  await assert.rejects(
    () => session.establish(),
    (error: unknown) => error instanceof ProvisioningError && error.reason === "wrong-pop",
  );
});

test("a wrong code is reported as such even though the device drops the link", async () => {
  // What the firmware does on a mismatch: fail the write, then disconnect.
  // Which one the client sees first is a race, so the link state must not
  // decide the verdict.
  const device = new EmulatedDevice("K3F9Q2M7ZX");
  device.dropOnCommand1 = true;
  const session = new ProvisioningSession(device, { pop: "abcd1234" });
  await assert.rejects(
    () => session.establish(),
    (error: unknown) => error instanceof ProvisioningError && error.reason === "wrong-pop",
  );
});

/** NetworkConfigPayload{ msg, <field> = body } */
const configReply = (field: number, body: Uint8Array) =>
  new Writer().enum(1, field - 10).message(field, body).finish();
const withStatus = (status: Status) => new Writer().enum(1, status).finish();

async function established(config: (request: Uint8Array) => Uint8Array): Promise<ProvisioningSession> {
  const device = new EmulatedDevice("K3F9Q2M7ZX");
  device.config = config;
  const session = new ProvisioningSession(device, { pop: "K3F9Q2M7ZX" });
  await session.establish();
  return session;
}

const isProtocolError = (error: unknown) => error instanceof ProvisioningError && error.reason === "protocol";

test("a status reply carrying an error is not taken for a connection", async () => {
  // network_config.c answers InternalError with no sta_state - which, with
  // Connected being 0, would otherwise read as success.
  const session = await established(() => configReply(11, withStatus(Status.InternalError)));
  await assert.rejects(() => session.waitForResult(undefined, 1000), isProtocolError);
});

test("a status reply without resp_get_status is refused", async () => {
  const session = await established(() => configReply(13, new Uint8Array()));
  await assert.rejects(() => session.waitForResult(undefined, 1000), isProtocolError);
});

test("an empty status reply is a connection", async () => {
  const session = await established(() => configReply(11, new Uint8Array()));
  const status = await session.waitForResult(undefined, 1000);
  assert.equal(status.status, Status.Success);
});

test("credentials the device refuses stop before the wait", async () => {
  let applied = false;
  const session = await established((request) => {
    let type = 0;
    readFields(request, (f) => { if (f.no === 1) type = Number(f.varint); });
    if (type === 2) return configReply(13, withStatus(Status.InvalidArgument)); // set_config
    applied = true;
    return configReply(15, new Uint8Array());
  });
  await assert.rejects(() => session.applyCredentials("Svermigo", "password"), isProtocolError);
  assert.equal(applied, false, "apply is not sent after a refused set");
});

test("credentials accepted by both steps go through", async () => {
  const session = await established((request) => {
    let type = 0;
    readFields(request, (f) => { if (f.no === 1) type = Number(f.varint); });
    return type === 2 ? configReply(13, new Uint8Array()) : configReply(15, new Uint8Array());
  });
  await session.applyCredentials("Svermigo", "password");
});

test("a failed apply is reported", async () => {
  const session = await established((request) => {
    let type = 0;
    readFields(request, (f) => { if (f.no === 1) type = Number(f.varint); });
    return type === 2 ? configReply(13, new Uint8Array()) : configReply(15, withStatus(Status.InternalError));
  });
  await assert.rejects(() => session.applyCredentials("Svermigo", "password"), isProtocolError);
});
