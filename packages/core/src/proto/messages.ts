/**
 * The protocomm messages pairble speaks, encoded against the vendored schemas
 * in ../../proto. Field numbers are quoted from those files; if a future
 * ESP-IDF changes one, the diff there is the place to look.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

import { asInt32, asUint32, readFields, Writer } from "./wire.js";

// Plain enums, not `const enum`: a const enum is erased from the emitted
// JavaScript, so a consumer that compiles per file - esbuild, Vite, anything
// with isolatedModules - reads undefined where it expects a number.
export enum SecScheme {
  Sec0 = 0,
  Sec1 = 1,
  Sec2 = 2,
}

export enum Status {
  Success = 0,
  InvalidSecScheme = 1,
  InvalidProto = 2,
  TooManySessions = 3,
  InvalidArgument = 4,
  InternalError = 5,
  CryptoError = 6,
  InvalidSession = 7,
}

export enum AuthMode {
  Open = 0,
  WEP = 1,
  WPA_PSK = 2,
  WPA2_PSK = 3,
  WPA_WPA2_PSK = 4,
  WPA2_Enterprise = 5,
  WPA3_PSK = 6,
  WPA2_WPA3_PSK = 7,
  // Beyond the schema, which stops at 7: the firmware copies ESP-IDF's
  // wifi_auth_mode_t straight into the field (network_scan.c), so newer
  // values arrive as they are.
  WAPI_PSK = 8,
  OWE = 9,
  WPA3_Enterprise_192 = 10,
  WPA3_Ext_PSK = 11,
  WPA3_Ext_PSK_Mixed = 12,
  DPP = 13,
  WPA3_Enterprise = 14,
  WPA2_WPA3_Enterprise = 15,
  WPA_Enterprise = 16,
}

export enum StationState {
  Connected = 0,
  Connecting = 1,
  Disconnected = 2,
  ConnectionFailed = 3,
}

export enum FailReason {
  AuthError = 0,
  NetworkNotFound = 1,
}

// ---------------------------------------------------------------- session ---

/** SessionData{ sec_ver=1, sec1=Sec1Payload{ msg=Session_Command0, sc0 } } */
export function sessionCmd0(clientPublicKey: Uint8Array): Uint8Array {
  const cmd0 = new Writer().bytes(1, clientPublicKey).finish();
  // msg = Session_Command0 = 0, which proto3 omits.
  const sec1 = new Writer().message(20, cmd0).finish();
  return new Writer().enum(2, SecScheme.Sec1).message(11, sec1).finish();
}

/** SessionData{ sec_ver=1, sec1=Sec1Payload{ msg=Session_Command1, sc1 } } */
export function sessionCmd1(clientVerify: Uint8Array): Uint8Array {
  const cmd1 = new Writer().bytes(2, clientVerify).finish();
  const sec1 = new Writer().enum(1, 2 /* Session_Command1 */).message(22, cmd1).finish();
  return new Writer().enum(2, SecScheme.Sec1).message(11, sec1).finish();
}

export interface SessionResp0 {
  status: Status;
  devicePublicKey: Uint8Array;
  deviceRandom: Uint8Array;
}

export interface SessionResp1 {
  status: Status;
  deviceVerify: Uint8Array;
}

function sec1Payload(sessionData: Uint8Array): Uint8Array {
  let payload = new Uint8Array();
  readFields(sessionData, (f) => {
    if (f.no === 11) payload = f.bytes.slice();
  });
  return payload;
}

export function parseSessionResp0(sessionData: Uint8Array): SessionResp0 {
  const out: SessionResp0 = {
    status: Status.Success,
    devicePublicKey: new Uint8Array(),
    deviceRandom: new Uint8Array(),
  };
  readFields(sec1Payload(sessionData), (f) => {
    if (f.no !== 21) return; // sr0
    readFields(f.bytes, (g) => {
      if (g.no === 1) out.status = asUint32(g.varint) as Status;
      else if (g.no === 2) out.devicePublicKey = g.bytes;
      else if (g.no === 3) out.deviceRandom = g.bytes;
    });
  });
  return out;
}

export function parseSessionResp1(sessionData: Uint8Array): SessionResp1 {
  const out: SessionResp1 = { status: Status.Success, deviceVerify: new Uint8Array() };
  readFields(sec1Payload(sessionData), (f) => {
    if (f.no !== 23) return; // sr1
    readFields(f.bytes, (g) => {
      if (g.no === 1) out.status = asUint32(g.varint) as Status;
      else if (g.no === 3) out.deviceVerify = g.bytes;
    });
  });
  return out;
}

// ------------------------------------------------------------------- scan ---

/** Defaults copied from esp_prov (tools/esp_prov/prov/wifi_scan.py), so the
 *  first thing pairble does matches what Espressif's own tooling does. */
export function scanStart(groupChannels = 5, periodMs = 120): Uint8Array {
  const cmd = new Writer()
    .bool(1, true)   // blocking
    .bool(2, false)  // passive
    .uint32(3, groupChannels)
    .uint32(4, periodMs)
    .finish();
  return new Writer().message(10, cmd).finish(); // msg = TypeCmdScanStart = 0
}

export function scanStatus(): Uint8Array {
  return new Writer().enum(1, 2 /* TypeCmdScanStatus */).message(12, new Uint8Array()).finish();
}

export function scanResult(startIndex: number, count: number): Uint8Array {
  const cmd = new Writer().uint32(1, startIndex).uint32(2, count).finish();
  return new Writer().enum(1, 4 /* TypeCmdScanResult */).message(14, cmd).finish();
}

export interface ScanStatus {
  finished: boolean;
  resultCount: number;
}

export function parseScanStatus(buf: Uint8Array): ScanStatus {
  const out: ScanStatus = { finished: false, resultCount: 0 };
  readFields(buf, (f) => {
    if (f.no !== 13) return; // resp_scan_status
    readFields(f.bytes, (g) => {
      if (g.no === 1) out.finished = g.varint !== 0n;
      else if (g.no === 2) out.resultCount = asUint32(g.varint);
    });
  });
  return out;
}

export interface ScanEntry {
  ssid: string;
  channel: number;
  rssi: number;
  bssid: Uint8Array;
  auth: AuthMode;
}

export function parseScanResult(buf: Uint8Array): ScanEntry[] {
  const entries: ScanEntry[] = [];
  readFields(buf, (f) => {
    if (f.no !== 15) return; // resp_scan_result
    readFields(f.bytes, (g) => {
      if (g.no !== 1) return; // repeated entries
      const entry: ScanEntry = {
        ssid: "",
        channel: 0,
        rssi: 0,
        bssid: new Uint8Array(),
        auth: AuthMode.Open,
      };
      readFields(g.bytes, (h) => {
        if (h.no === 1) entry.ssid = new TextDecoder().decode(h.bytes);
        else if (h.no === 2) entry.channel = asUint32(h.varint);
        else if (h.no === 3) entry.rssi = asInt32(h.varint); // negative: ten bytes
        else if (h.no === 4) entry.bssid = h.bytes;
        else if (h.no === 5) entry.auth = asUint32(h.varint) as AuthMode;
      });
      entries.push(entry);
    });
  });
  return entries;
}

// ----------------------------------------------------------------- config ---

export function setConfig(ssid: string, passphrase: string): Uint8Array {
  const encoder = new TextEncoder();
  const cmd = new Writer()
    .bytes(1, encoder.encode(ssid))
    .bytes(2, encoder.encode(passphrase))
    .finish();
  return new Writer().enum(1, 2 /* TypeCmdSetConfig */).message(12, cmd).finish();
}

export function applyConfig(): Uint8Array {
  return new Writer().enum(1, 4 /* TypeCmdApplyConfig */).message(14, new Uint8Array()).finish();
}

export function getStatus(): Uint8Array {
  return new Writer().message(10, new Uint8Array()).finish(); // TypeCmdGetStatus = 0
}

export interface DeviceStatus {
  status: Status;
  state: StationState;
  failReason?: FailReason;
  /** Present while the device is still retrying. This is what makes a useful
   *  progress indicator possible instead of a spinner: the wait for a failure
   *  is tens of seconds, and the device says how much of it is left. */
  attemptsRemaining?: number;
  connected?: { ip: string; ssid: string; channel: number; auth: AuthMode };
}

/** The status of a reply that carries nothing else - resp_set_config (13) and
 *  resp_apply_config (15). Null when the reply holds no such message at all.
 *  The message itself is a oneof member, so it is on the wire even when empty,
 *  which is what Success (0, omitted) looks like. */
function configStatus(buf: Uint8Array, field: number): Status | null {
  let status: Status | null = null;
  readFields(buf, (f) => {
    if (f.no !== field) return;
    status = Status.Success;
    readFields(f.bytes, (g) => {
      if (g.no === 1) status = asUint32(g.varint) as Status;
    });
  });
  return status;
}

export function parseSetConfigStatus(buf: Uint8Array): Status | null {
  return configStatus(buf, 13); // resp_set_config
}

export function parseApplyConfigStatus(buf: Uint8Array): Status | null {
  return configStatus(buf, 15); // resp_apply_config
}

/** Null when the reply carries no resp_get_status at all. */
export function parseStatus(buf: Uint8Array): DeviceStatus | null {
  // Connected is 0, and proto3 does not put a field at its default on the
  // wire - so a device that IS connected sends no sta_state at all. Defaulting
  // to anything else here means success is never observed, and the client
  // waits out its timeout on a device that is already online. The oneof
  // members below are exempt from that rule: presence is tracked, so
  // fail_reason = AuthError (also 0) does get serialised. The flip side: an
  // error reply (status != Success, nothing else) also reads as Connected, so
  // the caller must check `status` before believing `state`.
  let out: DeviceStatus | null = null;
  readFields(buf, (f) => {
    if (f.no !== 11) return; // resp_get_status
    const reply: DeviceStatus = { status: Status.Success, state: StationState.Connected };
    out = reply;
    readFields(f.bytes, (g) => {
      if (g.no === 1) reply.status = asUint32(g.varint) as Status;
      else if (g.no === 2) reply.state = asUint32(g.varint) as StationState;
      else if (g.no === 10) reply.failReason = asUint32(g.varint) as FailReason;
      else if (g.no === 12) {
        readFields(g.bytes, (h) => {
          if (h.no === 1) reply.attemptsRemaining = asUint32(h.varint);
        });
      } else if (g.no === 11) {
        const connected = { ip: "", ssid: "", channel: 0, auth: AuthMode.Open };
        readFields(g.bytes, (h) => {
          if (h.no === 1) connected.ip = new TextDecoder().decode(h.bytes);
          else if (h.no === 2) connected.auth = asUint32(h.varint) as AuthMode;
          else if (h.no === 3) connected.ssid = new TextDecoder().decode(h.bytes);
          else if (h.no === 5) connected.channel = asInt32(h.varint);
        });
        reply.connected = connected;
      }
    });
  });
  return out;
}
