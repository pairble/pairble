/**
 * Reads the device name and proof of possession the QR code carries.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Both the query and the fragment are accepted, and the fragment wins. The
 * fragment never leaves the browser, so a PoP carried there does not end up in
 * the host's access logs; it is what the firmware and the CLI emit, now that
 * App Clips are verified to receive it. The query is still read for codes
 * printed the older way, and for the "Open in Chrome" hand-off, which has to
 * move the fragment into the query (see chromeIntentUrl in env.ts).
 */

export interface DeviceParams {
  /** Advertised BLE name, e.g. "PAIRBLE-BA6550". Optional: without it the
   *  chooser filters by service UUID instead. */
  name: string | null;
  pop: string | null;
  /** 128-bit service UUID, lowercase, when the product uses its own. */
  serviceUuid: string | null;
  demo: boolean;
  /** Demo only: run the simulated timings five times faster. */
  fast: boolean;
  /** Demo only: pretend to be another environment, to review its screen. */
  demoEnv: string | null;
  /** Demo only: force the interface language. */
  demoLang: string | null;
}

const UUID = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/;

function clean(value: string | null, max: number): string | null {
  if (value === null) return null;
  // Strip control characters; a name or PoP never legitimately contains them.
  const text = value.replace(/[\u0000-\u001f\u007f]/g, "").trim().slice(0, max);
  return text.length > 0 ? text : null;
}

export function parseParams(search: string, hash: string): DeviceParams {
  const query = new URLSearchParams(search.startsWith("?") ? search.slice(1) : search);
  const fragment = new URLSearchParams(hash.startsWith("#") ? hash.slice(1) : hash);
  const get = (key: string) => fragment.get(key) ?? query.get(key);

  const svc = clean(get("svc"), 36)?.toLowerCase() ?? null;
  return {
    name: clean(get("name"), 29),  // the firmware's limits (pairble_text.c)
    pop: clean(get("pop"), 63),
    serviceUuid: svc !== null && UUID.test(svc) ? svc : null,
    demo: get("demo") === "1",
    fast: get("fast") === "1",
    demoEnv: clean(get("env"), 16),
    demoLang: clean(get("lang"), 8),
  };
}
