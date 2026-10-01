// SPDX-License-Identifier: Apache-2.0
import assert from "node:assert/strict";
import test from "node:test";
import { ProvisioningError } from "@pairble/core";
import { localConnect } from "../app/local.js";

type Reply = { status?: number; body?: unknown } | "network-error";

/** A fake device: each call to a path takes the next scripted reply. */
function device(script: Record<string, Reply[]>) {
  const calls: { path: string; init?: RequestInit }[] = [];
  const fakeFetch = (async (url: string, init?: RequestInit) => {
    const path = url.replace(/^.*?(\/api\/\w+).*$/, "$1");
    calls.push({ path, ...(init ? { init } : {}) });
    const queue = script[path] ?? [];
    const next = queue.length > 1 ? queue.shift()! : queue[0];
    if (next === undefined) throw new Error(`unscripted ${path}`);
    if (next === "network-error") throw new TypeError("Failed to fetch");
    return new Response(JSON.stringify(next.body ?? {}), { status: next.status ?? 200 });
  }) as typeof fetch;
  return { fetch: fakeFetch, calls };
}

const fast = { pollMs: 1, lostAfterMs: 50, sleep: () => Promise.resolve() };

test("scan retries while the device is busy scanning for someone else", async () => {
  const d = device({ "/api/scan": [{ status: 503 }, { status: 503 }, { body: [{ ssid: "Svermigo", rssi: -60, auth: 3 }] }] });
  const client = await localConnect({ fetch: d.fetch, ...fast })();
  assert.deepEqual(await client.scan(), [{ ssid: "Svermigo", rssi: -60, auth: 3 }]);
  assert.equal(d.calls.length, 3);
});

test("credentials go form-encoded, special characters intact", async () => {
  const d = device({ "/api/connect": [{ body: { ok: true } }] });
  const client = await localConnect({ fetch: d.fetch, ...fast })();
  await client.applyCredentials("Kavárna & spol.", "p@ss w+rd=%");
  const body = new URLSearchParams(String(d.calls[0]!.init!.body));
  assert.equal(body.get("ssid"), "Kavárna & spol.");
  assert.equal(body.get("password"), "p@ss w+rd=%");
  // The device refuses a POST without it; see CSRF_HEADER.
  assert.equal(new Headers(d.calls[0]!.init!.headers).get("X-Pairble"), "1");
});

test("a wrong password comes back as wrong-password", async () => {
  const d = device({ "/api/status": [{ body: { state: "connecting" } }, { body: { state: "failed", reason: "wrong-password" } }] });
  const client = await localConnect({ fetch: d.fetch, ...fast })();
  await assert.rejects(
    () => client.waitForResult(() => {}, 5000),
    (e: unknown) => e instanceof ProvisioningError && e.reason === "wrong-password",
  );
});

test("requests failing while the access point changes channel are ridden out", async () => {
  // Measured: the access point follows the device onto the router's channel.
  const d = device({
    "/api/status": [{ body: { state: "connecting" } }, "network-error", "network-error", { body: { state: "connected", ip: "192.168.1.154" } }],
  });
  const client = await localConnect({ fetch: d.fetch, ...fast, lostAfterMs: 10_000 })();
  assert.deepEqual(await client.waitForResult(() => {}, 5000), { ip: "192.168.1.154" });
});

test("contact lost for good ends as a timeout, not a hang", async () => {
  const d = device({ "/api/status": ["network-error"] });
  const client = await localConnect({ fetch: d.fetch, pollMs: 5, lostAfterMs: 20, sleep: (ms) => new Promise((r) => setTimeout(r, ms)) })();
  await assert.rejects(
    () => client.waitForResult(() => {}, 5000),
    (e: unknown) => e instanceof ProvisioningError && e.reason === "timeout",
  );
});
