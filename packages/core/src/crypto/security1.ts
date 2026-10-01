/**
 * Protocomm security scheme 1: X25519 key agreement, then AES-256-CTR.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Written against the firmware, not the documentation - see
 * components/protocomm/src/security/security1.c in ESP-IDF. Three details from
 * there decide whether this interoperates at all:
 *
 *  1. Keys travel little-endian. The device works in mbedtls MPIs, which are
 *     big-endian, and flips every key on the way in and out. What is on the
 *     wire is therefore ordinary X25519, and WebCrypto's raw format matches.
 *
 *  2. The proof of possession is folded in as a XOR, not a KDF:
 *     sym_key = X25519(shared) XOR SHA-256(pop).
 *
 *  3. The AES-CTR keystream is continuous for the whole session. The device
 *     keeps mbedtls's nc_off across calls, so the 32 bytes of the client
 *     verifier, the 32 bytes of the device verifier and every payload after
 *     them come off one unbroken stream. A payload of unaligned length leaves
 *     the next message starting mid-block, which is why this tracks a byte
 *     offset rather than calling encrypt() per message.
 */

const KEY_LEN = 32;

/**
 * Copies into a buffer this code owns. Not a type-checker appeasement: the
 * protobuf reader hands out subarray views onto the receive buffer, and
 * feeding those straight to WebCrypto would alias memory that the next packet
 * overwrites.
 */
function owned(data: Uint8Array) {
  const out = new Uint8Array(data.length);
  out.set(data);
  return out;
}

/** Big-endian 128-bit increment, matching mbedtls's CTR block counter. */
function counterForBlock(base: Uint8Array, block: number) {
  const out = owned(base);
  let carry = BigInt(block);
  for (let i = out.length - 1; i >= 0 && carry > 0n; i--) {
    const sum = BigInt(out[i]!) + (carry & 0xffn);
    out[i] = Number(sum & 0xffn);
    carry = (carry >> 8n) + (sum >> 8n);
  }
  return out;
}

/**
 * One direction-agnostic AES-CTR stream. CTR is its own inverse, so encrypt
 * and decrypt are the same operation; what matters is that both sides consume
 * the keystream in the same order.
 */
export class Sec1Cipher {
  private offset = 0;

  private constructor(
    private readonly key: CryptoKey,
    private readonly nonce: Uint8Array,
  ) {}

  static async create(symKey: Uint8Array, deviceRandom: Uint8Array): Promise<Sec1Cipher> {
    if (deviceRandom.length !== 16) {
      throw new Error(`sec1: device random must be 16 bytes, got ${deviceRandom.length}`);
    }
    const key = await crypto.subtle.importKey("raw", owned(symKey), "AES-CTR", false, ["encrypt"]);
    return new Sec1Cipher(key, owned(deviceRandom));
  }

  /** Consumes `data.length` bytes of keystream and XORs them in. */
  async xor(data: Uint8Array): Promise<Uint8Array> {
    if (data.length === 0) return new Uint8Array();

    const firstBlock = Math.floor(this.offset / 16);
    const skip = this.offset % 16;
    const blocks = Math.ceil((skip + data.length) / 16);

    // WebCrypto gives no raw block cipher, but AES-CTR over zeroes is exactly
    // the keystream, so this recovers the blocks we need.
    const keystream = new Uint8Array(
      await crypto.subtle.encrypt(
        { name: "AES-CTR", counter: counterForBlock(this.nonce, firstBlock), length: 128 },
        this.key,
        new Uint8Array(blocks * 16),
      ),
    );

    const out = new Uint8Array(data.length);
    for (let i = 0; i < data.length; i++) {
      out[i] = data[i]! ^ keystream[skip + i]!;
    }
    this.offset += data.length;
    return out;
  }
}

export interface Sec1Handshake {
  clientPublicKey: Uint8Array;
  /** Proves to the device that we derived the same key: the device's own
   *  public key, run through the cipher. */
  clientVerify(devicePublicKey: Uint8Array): Promise<Uint8Array>;
  /** Checks the device proved the same thing back. Throws on mismatch, which
   *  in practice means the proof of possession was wrong. */
  verifyDevice(deviceVerify: Uint8Array): Promise<void>;
  cipher(): Sec1Cipher;
}

export async function beginSec1(): Promise<{
  clientPublicKey: Uint8Array;
  complete: (devicePublicKey: Uint8Array, deviceRandom: Uint8Array, pop: string) => Promise<Sec1Handshake>;
}> {
  const pair = (await crypto.subtle.generateKey({ name: "X25519" }, false, [
    "deriveBits",
  ])) as CryptoKeyPair;
  const clientPublicKey = new Uint8Array(await crypto.subtle.exportKey("raw", pair.publicKey));

  const complete = async (
    devicePublicKey: Uint8Array,
    deviceRandom: Uint8Array,
    pop: string,
  ): Promise<Sec1Handshake> => {
    if (devicePublicKey.length !== KEY_LEN) {
      throw new Error(`sec1: device public key must be ${KEY_LEN} bytes`);
    }
    const devicePub = await crypto.subtle.importKey(
      "raw", owned(devicePublicKey), { name: "X25519" }, false, [],
    );
    const shared = new Uint8Array(
      await crypto.subtle.deriveBits({ name: "X25519", public: devicePub }, pair.privateKey, 256),
    );

    const symKey = owned(shared);
    if (pop.length > 0) {
      const digest = new Uint8Array(
        await crypto.subtle.digest("SHA-256", new TextEncoder().encode(pop)),
      );
      for (let i = 0; i < KEY_LEN; i++) symKey[i]! ^= digest[i]!;
    }

    const cipher = await Sec1Cipher.create(symKey, deviceRandom);

    return {
      clientPublicKey,
      clientVerify: (devPub) => cipher.xor(devPub),
      verifyDevice: async (deviceVerify) => {
        const decrypted = await cipher.xor(deviceVerify);
        const ok =
          decrypted.length === clientPublicKey.length &&
          decrypted.every((b, i) => b === clientPublicKey[i]);
        if (!ok) {
          throw new Error("sec1: device verification failed (wrong proof of possession?)");
        }
      },
      cipher: () => cipher,
    };
  };

  return { clientPublicKey, complete };
}
