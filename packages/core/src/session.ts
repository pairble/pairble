/**
 * A provisioning session: handshake, ask the device what it can see, hand it
 * credentials, then watch it try.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

import { beginSec1, type Sec1Cipher } from "./crypto/security1.js";
import * as msg from "./proto/messages.js";
import { SCAN_BATCH_BLE, SCAN_BATCH_HTTP, type Transport } from "./transport.js";

export type { ScanEntry, DeviceStatus } from "./proto/messages.js";
export { AuthMode, FailReason, StationState } from "./proto/messages.js";

export interface SessionOptions {
  /** Printed on the box and carried in the QR code. Unique per device: a
   *  shared one means anyone in radio range can provision any unit. */
  pop: string;
}

export type Progress =
  | { phase: "handshake" }
  | { phase: "scanning" }
  | { phase: "applying" }
  /** The device is retrying and says how many goes it has left. Worth showing:
   *  a failure can take half a minute to come back. */
  | { phase: "connecting"; attemptsRemaining?: number | undefined };

export class ProvisioningError extends Error {
  constructor(
    message: string,
    readonly reason: "wrong-password" | "network-not-found" | "wrong-pop" | "protocol" | "timeout",
  ) {
    super(message);
    this.name = "ProvisioningError";
  }
}

export class ProvisioningSession {
  private cipher?: Sec1Cipher;

  constructor(
    private readonly transport: Transport,
    private readonly options: SessionOptions,
  ) {}

  /** X25519 key agreement, then both sides prove they derived the same key.
   *  A wrong proof of possession fails here, not later. */
  async establish(): Promise<void> {
    const { clientPublicKey, complete } = await beginSec1();

    const resp0 = msg.parseSessionResp0(
      await this.transport.request("prov-session", msg.sessionCmd0(clientPublicKey)),
    );
    if (resp0.status !== msg.Status.Success) {
      throw new ProvisioningError(`session refused: status ${resp0.status}`, "protocol");
    }

    const handshake = await complete(resp0.devicePublicKey, resp0.deviceRandom, this.options.pop);
    const verify = await handshake.clientVerify(resp0.devicePublicKey);

    // A wrong proof of possession surfaces HERE, as a transport failure, not
    // as a mismatch further down. The firmware checks our verifier first and,
    // on mismatch, answers the write with an ATT error over BLE or an error
    // response over HTTP (security1.c, protocomm_nimble.c). Command 0 has just
    // succeeded, so the link and the protocol are fine and the PoP is the one
    // input left that can differ. Whether the link is still up afterwards says
    // nothing: on a mismatch the firmware also drops it ("Invalid content
    // received, killing connection"), so a check here would turn a wrong code
    // into "connection lost" whenever the drop wins the race.
    let reply1: Uint8Array;
    try {
      reply1 = await this.transport.request("prov-session", msg.sessionCmd1(verify));
    } catch {
      throw new ProvisioningError(
        "the code does not match this device - scan the code on the device again",
        "wrong-pop",
      );
    }
    const resp1 = msg.parseSessionResp1(reply1);
    if (resp1.status !== msg.Status.Success) {
      throw new ProvisioningError(`handshake rejected: status ${resp1.status}`, "protocol");
    }

    try {
      await handshake.verifyDevice(resp1.deviceVerify);
    } catch {
      // Only reachable if the device accepted our verifier yet proves a
      // different key back - not expected from a correct firmware, but the
      // same advice applies.
      throw new ProvisioningError(
        "the code does not match this device - scan the code on the device again",
        "wrong-pop",
      );
    }
    this.cipher = handshake.cipher();
  }

  /** Every endpoint past prov-session rides the session cipher, whose keystream
   *  is continuous - so these must not be issued concurrently. */
  private async secureRequest(endpoint: string, payload: Uint8Array): Promise<Uint8Array> {
    const cipher = this.cipher;
    if (!cipher) throw new ProvisioningError("session not established", "protocol");
    const response = await this.transport.request(endpoint, await cipher.xor(payload));
    return cipher.xor(response);
  }

  /**
   * The list comes from the device's own radio, which is the whole point: an
   * ESP32 tunes 2.4 GHz only, so a network it cannot join cannot appear.
   */
  async scan(): Promise<msg.ScanEntry[]> {
    await this.secureRequest("prov-scan", msg.scanStart());

    let status = msg.parseScanStatus(await this.secureRequest("prov-scan", msg.scanStatus()));
    for (let i = 0; i < 20 && !status.finished; i++) {
      await new Promise((resolve) => setTimeout(resolve, 200));
      status = msg.parseScanStatus(await this.secureRequest("prov-scan", msg.scanStatus()));
    }
    if (!status.finished) throw new ProvisioningError("scan did not finish", "timeout");

    const batch = this.transport.kind === "ble" ? SCAN_BATCH_BLE : SCAN_BATCH_HTTP;
    const entries: msg.ScanEntry[] = [];
    for (let index = 0; index < status.resultCount; index += batch) {
      const count = Math.min(batch, status.resultCount - index);
      entries.push(
        ...msg.parseScanResult(await this.secureRequest("prov-scan", msg.scanResult(index, count))),
      );
    }
    return entries.sort((a, b) => b.rssi - a.rssi);
  }

  /** Sends the credentials and tells the device to try them. */
  async applyCredentials(ssid: string, passphrase: string): Promise<void> {
    // Both replies carry a status. Ignoring it would send the person to the
    // "connecting" screen for a device that never took the credentials.
    const set = msg.parseSetConfigStatus(
      await this.secureRequest("prov-config", msg.setConfig(ssid, passphrase)),
    );
    if (set !== msg.Status.Success) {
      throw new ProvisioningError(`credentials refused: status ${set ?? "missing"}`, "protocol");
    }
    const apply = msg.parseApplyConfigStatus(
      await this.secureRequest("prov-config", msg.applyConfig()),
    );
    if (apply !== msg.Status.Success) {
      throw new ProvisioningError(`apply refused: status ${apply ?? "missing"}`, "protocol");
    }
  }

  /**
   * Polls until the device is on the network or has given up. Measured against
   * real firmware, a failure takes 15-26 s to arrive, so `onProgress` is not
   * decoration - without it the user is staring at nothing.
   */
  async waitForResult(
    onProgress?: (progress: Progress) => void,
    // Above the 54 s measured for a wrong password on firmware that retries
    // five times, so older devices are not cut off.
    timeoutMs = 90_000,
  ): Promise<msg.DeviceStatus> {
    const deadline = Date.now() + timeoutMs;
    for (;;) {
      const status = msg.parseStatus(await this.secureRequest("prov-config", msg.getStatus()));
      // Checked before the state: an error reply carries no sta_state, which
      // reads as Connected (0) - see parseStatus.
      if (status === null || status.status !== msg.Status.Success) {
        throw new ProvisioningError(
          `status query failed: status ${status?.status ?? "missing"}`,
          "protocol",
        );
      }

      switch (status.state) {
        case msg.StationState.Connected:
          return status;
        case msg.StationState.ConnectionFailed:
          throw status.failReason === msg.FailReason.NetworkNotFound
            ? new ProvisioningError(
                "the device could not find that network",
                "network-not-found",
              )
            : new ProvisioningError("wrong password", "wrong-password");
        default:
          onProgress?.({ phase: "connecting", attemptsRemaining: status.attemptsRemaining });
      }

      if (Date.now() > deadline) {
        throw new ProvisioningError("the device stopped reporting", "timeout");
      }
      await new Promise((resolve) => setTimeout(resolve, 1000));
    }
  }

  close(): Promise<void> {
    return this.transport.close();
  }
}
