/**
 * Minimal protobuf wire format: varints and length-delimited fields.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Hand-written rather than generated. The schemas pairble speaks are frozen
 * (ESP-IDF protocomm, vendored under proto/) and use a small corner of the
 * format: varint and length-delimited fields only - no floats, no groups, no
 * zigzag. A generated codec plus its runtime would cost more bundle than the
 * whole client, and this page has to fit in flash as well as on a CDN.
 *
 * The trade is that correctness lives here instead of in a library, so this
 * file carries tests and is checked against real firmware.
 */

export enum WireType {
  Varint = 0,
  LengthDelimited = 2,
}

export class Writer {
  private chunks: number[] = [];

  private varint(value: number | bigint): void {
    let v = BigInt(value);
    if (v < 0n) {
      // proto3 int32 holds negatives as a two's-complement 64-bit varint,
      // which is why a -60 dBm RSSI takes ten bytes on the wire.
      v += 1n << 64n;
    }
    do {
      const byte = Number(v & 0x7fn);
      v >>= 7n;
      this.chunks.push(v > 0n ? byte | 0x80 : byte);
    } while (v > 0n);
  }

  private tag(field: number, type: WireType): void {
    this.varint(field * 8 + type);
  }

  uint32(field: number, value: number): this {
    if (value !== 0) {
      this.tag(field, WireType.Varint);
      this.varint(value);
    }
    return this;
  }

  /** proto3 omits zero values; an enum at its default is simply not sent. */
  enum(field: number, value: number): this {
    return this.uint32(field, value);
  }

  bool(field: number, value: boolean): this {
    if (value) {
      this.tag(field, WireType.Varint);
      this.varint(1);
    }
    return this;
  }

  bytes(field: number, value: Uint8Array): this {
    if (value.length > 0) {
      this.tag(field, WireType.LengthDelimited);
      this.varint(value.length);
      for (const b of value) this.chunks.push(b);
    }
    return this;
  }

  string(field: number, value: string): this {
    return this.bytes(field, new TextEncoder().encode(value));
  }

  /** Nested messages are length-delimited, and an empty one still matters:
   *  CmdScanStatus carries no fields, yet its presence selects the oneof. */
  message(field: number, body: Uint8Array): this {
    this.tag(field, WireType.LengthDelimited);
    this.varint(body.length);
    for (const b of body) this.chunks.push(b);
    return this;
  }

  finish(): Uint8Array {
    return new Uint8Array(this.chunks);
  }
}

export interface Field {
  no: number;
  type: WireType;
  varint: bigint;
  bytes: Uint8Array;
}

export class Reader {
  private pos = 0;

  constructor(private readonly buf: Uint8Array) {}

  get done(): boolean {
    return this.pos >= this.buf.length;
  }

  private varint(): bigint {
    let result = 0n;
    let shift = 0n;
    for (;;) {
      if (this.pos >= this.buf.length) {
        throw new Error("protobuf: truncated varint");
      }
      const byte = this.buf[this.pos++]!;
      result |= BigInt(byte & 0x7f) << shift;
      if ((byte & 0x80) === 0) return result;
      shift += 7n;
      if (shift > 63n) throw new Error("protobuf: varint too long");
    }
  }

  next(): Field {
    const tag = this.varint();
    const no = Number(tag >> 3n);
    const type = Number(tag & 7n) as WireType;
    if (type === WireType.Varint) {
      return { no, type, varint: this.varint(), bytes: new Uint8Array() };
    }
    if (type === WireType.LengthDelimited) {
      const len = Number(this.varint());
      if (this.pos + len > this.buf.length) {
        throw new Error("protobuf: truncated length-delimited field");
      }
      const bytes = this.buf.subarray(this.pos, this.pos + len);
      this.pos += len;
      return { no, type, varint: 0n, bytes };
    }
    throw new Error(`protobuf: unsupported wire type ${type}`);
  }
}

/** Walks a message, handing each field to `visit`. Unknown fields are skipped,
 *  which is what lets a newer firmware add fields without breaking this client. */
export function readFields(buf: Uint8Array, visit: (field: Field) => void): void {
  const reader = new Reader(buf);
  while (!reader.done) visit(reader.next());
}

/** proto3 int32: the wire carries a 64-bit two's-complement value. */
export function asInt32(v: bigint): number {
  const masked = BigInt.asIntN(32, BigInt.asUintN(64, v));
  return Number(masked);
}

export function asUint32(v: bigint): number {
  return Number(BigInt.asUintN(32, v));
}
