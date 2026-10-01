/**
 * The page as served by the device itself, over its own access point.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * No protocomm here. The access point is WPA2 with the proof of possession as
 * its password, so a phone that can reach this page has already proved it
 * holds the label, and the link is encrypted. The device exposes a plain API:
 * GET /api/info, GET /api/scan, POST /api/connect, GET /api/status.
 */

import { ProvisioningError } from "@pairble/core";
import type { Client, Connect, JoinProgress, ScanResult } from "./client.js";

export interface LocalOptions {
  fetch?: typeof fetch;
  base?: string;
  pollMs?: number;
  /** How long to keep polling through failed requests before giving up. The
   *  access point follows the device onto the router's channel when it
   *  connects; an iPhone rides that out, but requests can fail meanwhile. */
  lostAfterMs?: number;
  sleep?: (ms: number) => Promise<void>;
}

interface Status {
  state: "idle" | "connecting" | "failed" | "connected";
  reason?: "wrong-password" | "network-not-found";
  ip?: string;
}

/** Sent with every POST. The device refuses a POST without it: a custom header
 *  makes the browser ask first (a CORS preflight) before any cross-origin
 *  request, which the device never approves, so another page the phone has
 *  open cannot submit credentials to it behind the person's back. */
export const CSRF_HEADER = { "X-Pairble": "1" } as const;

export async function fetchDeviceName(opts: LocalOptions = {}): Promise<string | null> {
  const f = opts.fetch ?? fetch;
  try {
    const res = await f(`${opts.base ?? ""}/api/info`, { cache: "no-store" });
    if (!res.ok) return null;
    const info = (await res.json()) as { name?: unknown };
    return typeof info.name === "string" ? info.name : null;
  } catch {
    return null;
  }
}

export function localConnect(opts: LocalOptions = {}): Connect {
  const f = opts.fetch ?? fetch;
  const base = opts.base ?? "";
  const pollMs = opts.pollMs ?? 1000;
  const lostAfterMs = opts.lostAfterMs ?? 20_000;
  const sleep = opts.sleep ?? ((ms: number) => new Promise<void>((r) => setTimeout(r, ms)));

  const client: Client = {
    async scan(): Promise<ScanResult[]> {
      // 503 means the device is already scanning for someone else; its scan
      // takes a few seconds, so a handful of retries covers it.
      for (let attempt = 0; attempt < 5; attempt++) {
        const res = await f(`${base}/api/scan`, { cache: "no-store" });
        if (res.status === 503) {
          await sleep(1500);
          continue;
        }
        if (!res.ok) throw new ProvisioningError(`scan failed: HTTP ${res.status}`, "protocol");
        return (await res.json()) as ScanResult[];
      }
      throw new ProvisioningError("the device stayed busy", "timeout");
    },

    async applyCredentials(ssid: string, passphrase: string): Promise<void> {
      const res = await f(`${base}/api/connect`, {
        method: "POST",
        headers: { "Content-Type": "application/x-www-form-urlencoded", ...CSRF_HEADER },
        body: new URLSearchParams({ ssid, password: passphrase }).toString(),
        cache: "no-store",
      });
      if (!res.ok) throw new ProvisioningError(`connect refused: HTTP ${res.status}`, "protocol");
    },

    async waitForResult(onProgress: (p: JoinProgress) => void, timeoutMs: number): Promise<unknown> {
      const deadline = Date.now() + timeoutMs;
      let failingSince: number | null = null;
      for (;;) {
        let status: Status | null = null;
        try {
          const res = await f(`${base}/api/status`, { cache: "no-store" });
          if (res.ok) status = (await res.json()) as Status;
        } catch {
          status = null;
        }

        if (status === null) {
          failingSince ??= Date.now();
          if (Date.now() - failingSince > lostAfterMs) {
            throw new ProvisioningError("lost contact with the device", "timeout");
          }
        } else {
          failingSince = null;
          if (status.state === "connected") return { ip: status.ip };
          if (status.state === "failed") {
            throw status.reason === "wrong-password"
              ? new ProvisioningError("wrong password", "wrong-password")
              : new ProvisioningError("network not found", "network-not-found");
          }
          onProgress({ phase: "connecting" });
        }

        if (Date.now() > deadline) throw new ProvisioningError("no verdict in time", "timeout");
        await sleep(pollMs);
      }
    },

    async close(): Promise<void> {
      // Nothing to hold open: every request stands alone.
    },
  };

  return async () => client;
}
