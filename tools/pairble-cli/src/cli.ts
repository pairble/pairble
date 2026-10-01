#!/usr/bin/env node
/**
 * pairble - production-line tooling for pairble devices.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

import { mkdirSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import QRCode from "qrcode";
import { deviceNameFromMac, generatePop, nvsCsv, setupUrl, validatePop, wifiQr } from "./lib.js";

const DEFAULT_BASE = "https://pairble.com/s";

const USAGE = `pairble - production-line tooling for pairble devices

  pairble unit --mac <mac> [--pop <pop>] [--base <url>] [--prefix PAIRBLE-] [--out <dir>]
      Everything for one unit: a new PoP (unless given), the device name, the
      setup URL, qr.svg, nvs.csv, and the commands to put the PoP on the chip.

  pairble pop                       print a new proof of possession
  pairble name <mac> [--prefix P]   device name the firmware will advertise
  pairble url --mac <mac> --pop <pop> [--base <url>] [--prefix P]
  pairble qr  --mac <mac> --pop <pop> [--base <url>] [--prefix P] [--svg <file>]

The MAC is the base MAC printed by: esptool.py --port <port> read_mac
`;

function flags(argv: string[]): Record<string, string> {
  const out: Record<string, string> = {};
  for (let i = 0; i < argv.length; i++) {
    const arg = argv[i]!;
    if (!arg.startsWith("--")) continue;
    const value = argv[i + 1];
    if (value === undefined || value.startsWith("--")) throw new Error(`${arg} needs a value`);
    out[arg.slice(2)] = value;
    i++;
  }
  return out;
}

function need(f: Record<string, string>, key: string): string {
  const v = f[key];
  if (!v) throw new Error(`--${key} is required`);
  return v;
}

async function main(argv: string[]): Promise<void> {
  const [command, ...rest] = argv;
  const f = flags(rest);
  const prefix = f.prefix ?? "PAIRBLE-";
  const base = f.base ?? DEFAULT_BASE;

  switch (command) {
    case "pop":
      console.log(generatePop());
      return;

    case "name":
      console.log(deviceNameFromMac(rest[0] ?? "", prefix));
      return;

    case "url": {
      const pop = need(f, "pop");
      console.log(setupUrl({ base, name: deviceNameFromMac(need(f, "mac"), prefix), pop }));
      return;
    }

    case "qr": {
      const url = setupUrl({ base, name: deviceNameFromMac(need(f, "mac"), prefix), pop: need(f, "pop") });
      console.log(await QRCode.toString(url, { type: "terminal", small: true, errorCorrectionLevel: "M" }));
      console.log(url);
      if (f.svg) writeFileSync(f.svg, await QRCode.toString(url, { type: "svg", margin: 2, errorCorrectionLevel: "M" }));
      return;
    }

    case "unit": {
      const name = deviceNameFromMac(need(f, "mac"), prefix);
      const pop = f.pop ?? generatePop();
      validatePop(pop);
      const url = setupUrl({ base, name, pop });
      const out = f.out ?? name;
      mkdirSync(out, { recursive: true });
      // M rather than L: labels get scuffed, and the URL is short enough that
      // the extra redundancy costs no QR version.
      writeFileSync(join(out, "qr.svg"), await QRCode.toString(url, { type: "svg", margin: 2, errorCorrectionLevel: "M" }));
      // The small second code: joins the device's own network, for phones
      // that cannot use Bluetooth.
      const wifi = wifiQr({ name, pop });
      writeFileSync(join(out, "wifi-qr.svg"), await QRCode.toString(wifi, { type: "svg", margin: 2, errorCorrectionLevel: "M" }));
      writeFileSync(join(out, "nvs.csv"), nvsCsv(pop));
      writeFileSync(join(out, "unit.json"), JSON.stringify({ name, pop, url, wifi }, null, 2) + "\n");
      console.log(`${name}\n  url     ${url}\n  wifi    ${wifi}\n  files   ${out}/qr.svg, wifi-qr.svg, nvs.csv, unit.json\n`);
      console.log("Put the PoP on the chip (default partition table: nvs at 0x9000, 0x6000 bytes):");
      console.log(`  python $IDF_PATH/components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py generate ${out}/nvs.csv ${out}/nvs.bin 0x6000`);
      console.log(`  esptool.py --port <port> write_flash 0x9000 ${out}/nvs.bin`);
      console.log("\nunit.json holds the PoP in clear text. Keep it with the production records, not in the repository.");
      return;
    }

    default:
      process.stdout.write(USAGE);
      if (command && command !== "help" && command !== "--help") process.exitCode = 1;
  }
}

main(process.argv.slice(2)).catch((error: Error) => {
  console.error(`pairble: ${error.message}`);
  process.exitCode = 1;
});
