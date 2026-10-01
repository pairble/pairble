/**
 * Production-line helpers: per-unit proof of possession, device name, setup
 * URL and the NVS content that carries the PoP onto the device.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

import { webcrypto } from "node:crypto";

/**
 * Crockford base32 minus the letters people misread off a label (I, L, O, U).
 * 32 symbols, so one random byte masked to five bits is unbiased.
 *
 * Why twelve characters: security 1 keeps a passive listener from testing
 * guesses offline - the session key needs an X25519 private key - but not a
 * rogue device that advertises this unit's name. A phone that connects to it
 * completes command 0 and sends a verifier derived from the PoP, and the
 * impostor can test PoP guesses against that at leisure. The SoftAP fallback
 * gives away the same thing: one captured WPA2 handshake with the PoP as the
 * password. So the PoP has to stand up to an offline search: twelve symbols
 * is 60 bits, up from the 50 of the ten used before - expensive to search,
 * though not beyond a determined attacker - and still short enough to type
 * from a sticker if the QR code is damaged.
 */
const ALPHABET = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

export function generatePop(length = 12): string {
  if (length < 8 || length > 63) throw new Error("PoP length must be 8-63");
  const bytes = webcrypto.getRandomValues(new Uint8Array(length));
  return Array.from(bytes, (b) => ALPHABET[b & 31]).join("");
}

/** The firmware's rule (pairble_valid_pop in pairble_text.c): it travels
 *  through a URL and a CSV file, and doubles as the WPA2 password of the
 *  SoftAP fallback - a passphrase, so 8 to 63 characters (64 would be read as
 *  a raw hex key). */
export function validatePop(pop: string): void {
  if (!/^[A-Za-z0-9._~-]{8,63}$/.test(pop)) {
    throw new Error("PoP must be 8-63 characters of A-Z, a-z, 0-9 or ._~-");
  }
}

/** PAIRBLE_NAME_MAX_LEN in pairble.h. The prefix is followed by six hex
 *  digits of the MAC, which leaves it 23. */
const NAME_MAX = 29;
const PREFIX_MAX = NAME_MAX - 6;

/** The firmware's rule (pairble_valid_name in pairble_text.c): the name goes
 *  into the BLE advertisement, the SoftAP SSID and the URL, so it keeps to
 *  characters that are safe in all three. */
export function validateName(name: string): void {
  if (!new RegExp(`^[A-Za-z0-9_-]{1,${NAME_MAX}}$`).test(name)) {
    throw new Error(`name must be 1-${NAME_MAX} characters of A-Z, a-z, 0-9, - or _`);
  }
}

/** Mirrors the firmware: "<prefix><last three bytes of the Wi-Fi STA MAC>".
 *  On ESP32-family chips that is the base MAC, which `esptool.py read_mac`
 *  prints. */
export function deviceNameFromMac(mac: string, prefix = "PAIRBLE-"): string {
  if (!new RegExp(`^[A-Za-z0-9_-]{0,${PREFIX_MAX}}$`).test(prefix)) {
    throw new Error(`prefix must be at most ${PREFIX_MAX} characters of A-Z, a-z, 0-9, - or _`);
  }
  const bytes = mac.trim().split(/[:-]/);
  if (bytes.length !== 6 || !bytes.every((b) => /^[0-9a-fA-F]{2}$/.test(b))) {
    throw new Error(`not a MAC address: ${mac}`);
  }
  return prefix + bytes.slice(3).join("").toUpperCase();
}

/** The URL behind the QR code. The parameters ride in the fragment, which a
 *  browser never sends to the server, so the proof of possession stays out of
 *  the host's access logs. An App Clip receives the fragment of its invocation
 *  URL - verified on a device. The clients also still
 *  accept query parameters, so codes already printed that way keep working. */
export function setupUrl(opts: { base: string; name: string; pop: string }): string {
  validateName(opts.name);
  validatePop(opts.pop);
  const url = new URL(opts.base);
  url.hash = new URLSearchParams({ name: opts.name, pop: opts.pop }).toString();
  return url.toString();
}

/** The second QR code, for the SoftAP fallback: scanning it joins the
 *  device's own network. Same format the firmware's pairble_wifi_qr() returns.
 *  Escaping per the WIFI: QR convention, although names and PoPs that pass
 *  validation never contain the characters that need it. */
export function wifiQr(opts: { name: string; pop: string }): string {
  validatePop(opts.pop);
  const esc = (v: string) => v.replace(/([\\;,:"])/g, "\\$1");
  return `WIFI:S:${esc(opts.name)};T:WPA;P:${esc(opts.pop)};;`;
}

/** Input for ESP-IDF's nvs_partition_gen.py: namespace "pairble", key "pop",
 *  which is where the firmware looks. */
export function nvsCsv(pop: string): string {
  validatePop(pop);
  return ["key,type,encoding,value", "pairble,namespace,,", `pop,data,string,${pop}`, ""].join("\n");
}
