/**
 * Web Bluetooth transport for protocomm.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Only usable from a secure context - HTTPS or http://localhost. On plain
 * http:// and on private addresses such as 192.168.4.1 `navigator.bluetooth`
 * is undefined, which is why the BLE page is hosted and the copy served from
 * the device's own flash talks HTTP instead.
 */

import type { Transport } from "@pairble/core";

/** The UUID the ESP-IDF provisioning example advertises. Products normally set
 *  their own with wifi_prov_scheme_ble_set_service_uuid(). */
export const ESP_PROV_EXAMPLE_SERVICE = "021a9004-0382-4aea-bff4-6b3f1c5adfb4";

/** Characteristic User Description. Endpoints are found by reading this off
 *  each characteristic rather than by deriving UUIDs: ESP-IDF assigns bytes 12
 *  and 13 of the service UUID automatically and documents the descriptor as
 *  the way clients are meant to identify endpoints. */
const USER_DESCRIPTION = 0x2901;

export interface BleConnectOptions {
  /** 128-bit service UUID, lowercase. Must match the firmware. */
  serviceUuid?: string;
  /** Narrows the chooser so the user sees one device rather than the street.
   *  Print the full name on the box. */
  namePrefix?: string;
  /** Called when the link drops for any reason - out of range, device reset,
   *  or the firmware dropping an idle client so other phones can find it. */
  onDisconnect?: () => void;
}

export class BleTransport implements Transport {
  readonly kind = "ble" as const;

  private constructor(
    private readonly device: BluetoothDevice,
    private readonly endpoints: Map<string, BluetoothRemoteGATTCharacteristic>,
  ) {}

  /**
   * Opens the browser's device chooser. Must be called from a user gesture -
   * a click or a tap - or the browser rejects it; there is no way to scan
   * silently, by design.
   */
  static async connect(options: BleConnectOptions = {}): Promise<BleTransport> {
    const serviceUuid = options.serviceUuid ?? ESP_PROV_EXAMPLE_SERVICE;

    if (typeof navigator === "undefined" || !("bluetooth" in navigator)) {
      throw new Error(
        "Web Bluetooth is unavailable. Chrome or Samsung Internet on Android over HTTPS; " +
          "not Safari, not Firefox, and not an in-app browser built on Android WebView.",
      );
    }

    const device = await navigator.bluetooth.requestDevice({
      filters: options.namePrefix
        ? [{ namePrefix: options.namePrefix }]
        : [{ services: [serviceUuid] }],
      optionalServices: [serviceUuid],
    });

    const server = await device.gatt?.connect();
    const onDisconnect = options.onDisconnect;
    if (onDisconnect) {
      // Removes itself, i.e. { once: true } - which the Web Bluetooth type
      // definitions do not accept on this overload, although browsers do.
      const gone = () => {
        device.removeEventListener("gattserverdisconnected", gone);
        onDisconnect();
      };
      device.addEventListener("gattserverdisconnected", gone);
    }
    if (!server) throw new Error("GATT server unavailable");

    const endpoints = new Map<string, BluetoothRemoteGATTCharacteristic>();
    try {
      const service = await server.getPrimaryService(serviceUuid);
      for (const characteristic of await service.getCharacteristics()) {
        try {
          const descriptor = await characteristic.getDescriptor(USER_DESCRIPTION);
          const name = new TextDecoder().decode(await descriptor.readValue()).replace(/\0+$/, "");
          if (name) endpoints.set(name, characteristic);
        } catch {
          // A characteristic without the descriptor is not an endpoint.
        }
      }
    } catch (error) {
      // Not our device, or the link dropped mid-discovery. Either way let go:
      // while a phone holds the connection the device stops advertising, and
      // nobody - this page included - could find it again.
      server.disconnect();
      throw error;
    }

    if (!endpoints.has("prov-session")) {
      await server.disconnect();
      throw new Error(
        `no prov-session endpoint on ${serviceUuid} - wrong service UUID, or not a provisioning device`,
      );
    }

    return new BleTransport(device, endpoints);
  }

  get endpointNames(): string[] {
    return [...this.endpoints.keys()];
  }

  async request(endpoint: string, payload: Uint8Array): Promise<Uint8Array> {
    const characteristic = this.endpoints.get(endpoint);
    if (!characteristic) {
      throw new Error(`endpoint ${endpoint} not offered by this device`);
    }
    // protocomm is request/response on one characteristic: write, then read
    // the reply back off the same handle. Payloads above the MTU are split by
    // the platform's long write and read-blob procedures, so there is nothing
    // to fragment here.
    await characteristic.writeValueWithResponse(payload as BufferSource);
    const response = await characteristic.readValue();
    return new Uint8Array(response.buffer.slice(response.byteOffset, response.byteOffset + response.byteLength));
  }

  async close(): Promise<void> {
    this.device.gatt?.disconnect();
  }
}
