/**
 * Maps whatever went wrong onto what the person should do next.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Errors arrive from three layers - Web Bluetooth DOMExceptions, the core
 * ProvisioningError, and plain Errors from the handshake - and none of them
 * is fit to show as-is. Every outcome here names a screen and an action.
 */

export type Outcome =
  /** The chooser was dismissed. Not a failure; offer Connect again. */
  | "cancelled"
  | "bluetooth-off"
  /** No device matched the filter, or it lacked the provisioning service. */
  | "no-device"
  /** The Security1 handshake failed its verification: the PoP from the QR
   *  code does not belong to this device. */
  | "wrong-code"
  | "wrong-password"
  | "network-not-found"
  | "lost"
  | "timeout"
  | "unknown";

export function classifyError(error: unknown): Outcome {
  const e = error as { name?: string; message?: string; reason?: string } | null;
  const name = e?.name ?? "";
  const message = (e?.message ?? "").toLowerCase();
  const reason = e?.reason;

  if (name === "ProvisioningError") {
    if (reason === "wrong-password") return "wrong-password";
    if (reason === "network-not-found") return "network-not-found";
    if (reason === "timeout") return "timeout";
    if (reason === "wrong-pop") return "wrong-code";
  }
  if (name === "NotFoundError") {
    if (message.includes("cancel")) return "cancelled";
    if (message.includes("adapter")) return "bluetooth-off";
    return "no-device";
  }
  if (message.includes("adapter") || message.includes("bluetooth is off")) return "bluetooth-off";
  if (message.includes("no prov-session endpoint")) return "no-device";
  if (
    name === "NetworkError" ||
    message.includes("disconnected") ||
    message.includes("gatt") ||
    message.includes("failed to fetch")
  ) {
    return "lost";
  }
  return "unknown";
}
