/**
 * What the UI needs from a provisioning session, whatever carries it.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ProvisioningSession from @pairble/core satisfies this directly; the demo
 * implements it with timers. Keeping the UI on this narrow surface is what
 * keeps the demo out of the real code path.
 */

export interface ScanResult {
  ssid: string;
  rssi: number;
  auth: number;
}

/** Mirrors core's Progress loosely; the UI only reads the attempt count. */
export interface JoinProgress {
  phase?: string;
  attemptsRemaining?: number | undefined;
}

export interface Client {
  scan(): Promise<ScanResult[]>;
  applyCredentials(ssid: string, passphrase: string): Promise<void>;
  waitForResult(onProgress: (p: JoinProgress) => void, timeoutMs: number): Promise<unknown>;
  close(): Promise<void>;
}

export type Connect = () => Promise<Client>;
