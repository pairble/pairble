/**
 * Turns the device's scan into the list a person chooses from.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

import { AuthMode } from "@pairble/core";

/** WifiAuthMode from the protocomm schema, re-exported under the name the rest
 *  of the app uses. */
export const Auth = AuthMode;

export interface ScanLike {
  ssid: string;
  rssi: number;
  auth: number;
}

export interface NetworkRow {
  ssid: string;
  rssi: number;
  auth: number;
  bars: 1 | 2 | 3 | 4;
  /** Asks for a password. */
  secured: boolean;
  /** Enterprise networks want a username and certificate, which this flow
   *  has no way to collect. Shown, but not selectable. */
  supported: boolean;
}

/** Every flavour of 802.1X ESP-IDF reports. */
const ENTERPRISE = new Set<number>([
  Auth.WPA2_Enterprise, Auth.WPA3_Enterprise_192, Auth.WPA3_Enterprise, Auth.WPA2_WPA3_Enterprise, Auth.WPA_Enterprise,
]);

/** Nothing to type: OWE encrypts the link but, like an open network, takes
 *  no password. */
const NO_PASSWORD = new Set<number>([Auth.Open, Auth.OWE]);

export function signalBars(rssi: number): 1 | 2 | 3 | 4 {
  if (rssi >= -55) return 4;
  if (rssi >= -67) return 3;
  if (rssi >= -78) return 2;
  return 1;
}

/**
 * Hidden networks (empty SSID) are dropped - they can still be joined through
 * "Other network". Several access points broadcasting one SSID (a mesh, or a
 * router plus repeater) collapse into one row carrying the strongest signal,
 * because to the person choosing they are one network.
 */
export function prepareNetworks(entries: readonly ScanLike[]): NetworkRow[] {
  const best = new Map<string, ScanLike>();
  for (const entry of entries) {
    if (entry.ssid.trim() === "") continue;
    const seen = best.get(entry.ssid);
    if (seen === undefined || entry.rssi > seen.rssi) best.set(entry.ssid, entry);
  }
  return [...best.values()]
    .sort((a, b) => b.rssi - a.rssi || a.ssid.localeCompare(b.ssid))
    .map((e) => ({
      ssid: e.ssid,
      rssi: e.rssi,
      auth: e.auth,
      bars: signalBars(e.rssi),
      // Anything not known to be open asks for a password: a mode newer than
      // this list is far more likely to want one than not.
      secured: !NO_PASSWORD.has(e.auth),
      supported: !ENTERPRISE.has(e.auth),
    }));
}

const PSK = new Set<number>([
  Auth.WPA_PSK, Auth.WPA2_PSK, Auth.WPA_WPA2_PSK, Auth.WPA3_PSK, Auth.WPA2_WPA3_PSK,
  Auth.WPA3_Ext_PSK, Auth.WPA3_Ext_PSK_Mixed,
]);

/** Wi-Fi limits are in bytes, and a form field counts UTF-16 units: "ž" is
 *  one unit but two bytes. */
const byteLength = (text: string) => new TextEncoder().encode(text).length;

/**
 * Catches what can be caught before spending twenty seconds finding out: a
 * WPA passphrase is 8 to 63 bytes. (A raw 64-hex-digit key is valid WPA, but
 * the device's Bluetooth path stores at most 63 bytes, so it is refused here
 * too.) `auth` is null for a network typed in by hand, where the type is not
 * known and only the upper limit, which no network exceeds, is checked.
 */
export function passwordProblem(auth: number | null, password: string): "too-short" | "too-long" | null {
  const bytes = byteLength(password);
  if (bytes > 63) return "too-long";
  if (auth !== null && PSK.has(auth) && bytes < 8) return "too-short";
  return null;
}

/** For a name typed in by hand. Spaces are kept - they are part of an SSID -
 *  but a name of nothing else is no name. At most 32 bytes. */
export function ssidProblem(ssid: string): "empty" | "too-long" | null {
  if (ssid.trim() === "") return "empty";
  if (byteLength(ssid) > 32) return "too-long";
  return null;
}
