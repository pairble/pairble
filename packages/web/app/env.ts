/**
 * Decides, before anything is tapped, whether this page can do its job here.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Feature detection first: a secure context with navigator.bluetooth is the
 * only thing that makes the BLE path work, whatever the user agent says. The
 * user agent is consulted only to word the advice when that path is missing -
 * "use the Camera app" helps on an iPhone and is nonsense on a laptop.
 */

export interface EnvInput {
  hasBluetooth: boolean;
  secure: boolean;
  protocol: string;
  hostname: string;
  userAgent: string;
  maxTouchPoints: number;
}

export type Env =
  /** Hosted over HTTPS in a browser with Web Bluetooth. */
  | { kind: "ble" }
  /** Served by the device itself over its own access point. */
  | { kind: "http" }
  /** Plain http:// on a real hostname: Bluetooth is withheld until https. */
  | { kind: "insecure" }
  | { kind: "ios" }
  | { kind: "android"; browser: "webview" | "firefox" | "other" }
  | { kind: "desktop" };

function isDeviceHost(hostname: string): boolean {
  return /^\d{1,3}(\.\d{1,3}){3}$/.test(hostname) || hostname.endsWith(".local");
}

export function detectEnvironment(input: EnvInput): Env {
  if (input.hasBluetooth && input.secure) return { kind: "ble" };
  // Checked before the platform: the device-served copy talks HTTP and works
  // on any phone, iPhones included.
  if (input.protocol === "http:" && isDeviceHost(input.hostname)) return { kind: "http" };
  if (input.protocol === "http:" && input.hostname !== "localhost") return { kind: "insecure" };

  const ua = input.userAgent;
  // iPadOS reports itself as a Mac; touch support gives it away.
  if (/iPhone|iPad|iPod/.test(ua) || (/Macintosh/.test(ua) && input.maxTouchPoints > 1)) {
    return { kind: "ios" };
  }
  if (/Android/.test(ua)) {
    if (/; wv\)/.test(ua)) return { kind: "android", browser: "webview" };
    if (/Firefox\//.test(ua)) return { kind: "android", browser: "firefox" };
    return { kind: "android", browser: "other" };
  }
  return { kind: "desktop" };
}

export function readEnvironment(): EnvInput {
  return {
    hasBluetooth: typeof navigator !== "undefined" && "bluetooth" in navigator,
    secure: typeof isSecureContext !== "undefined" && isSecureContext,
    protocol: location.protocol,
    hostname: location.hostname,
    userAgent: navigator.userAgent,
    maxTouchPoints: navigator.maxTouchPoints ?? 0,
  };
}

/**
 * An Android intent that reopens this page in Chrome. Everything before
 * "#Intent" is the URL, so a fragment of our own cannot survive inside it;
 * fragment parameters are folded into the query for this one hop. A known
 * trade-off: for this hop alone, the PoP (and the name) reach the host's
 * access logs - the alternative is a page that cannot connect at all.
 */
export function chromeIntentUrl(href: string): string {
  const url = new URL(href);
  const merged = new URLSearchParams(url.search);
  new URLSearchParams(url.hash.slice(1)).forEach((value, key) => merged.set(key, value));
  const query = merged.toString();
  return `intent://${url.host}${url.pathname}${query ? `?${query}` : ""}#Intent;scheme=https;package=com.android.chrome;end`;
}

/** The same page over https, for the "insecure" case. */
export function secureUrl(href: string): string {
  const url = new URL(href);
  url.protocol = "https:";
  return url.toString();
}
