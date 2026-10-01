// SPDX-License-Identifier: Apache-2.0
import assert from "node:assert/strict";
import test from "node:test";
import { deviceNameFromMac, generatePop, nvsCsv, setupUrl, validateName, validatePop, wifiQr } from "../src/lib.js";

test("device name matches what the firmware advertised on real hardware", () => {
  // The ESP32-S3 used in testing: STA MAC 20:6e:f1:ba:65:50 advertises
  // PAIRBLE-BA6550, and PROV_BA6550 with the prefix Espressif's examples use.
  assert.equal(deviceNameFromMac("20:6e:f1:ba:65:50"), "PAIRBLE-BA6550");
  assert.equal(deviceNameFromMac("20:6e:f1:ba:65:50", "PROV_"), "PROV_BA6550");
  assert.equal(deviceNameFromMac("20-6E-F1-BA-65-50", "LAMP_"), "LAMP_BA6550");
  assert.throws(() => deviceNameFromMac("20:6e:f1:ba:65"), /not a MAC/);
});

test("names and prefixes the firmware would refuse are refused here first", () => {
  // PAIRBLE_NAME_MAX_LEN is 29, and the MAC adds six characters.
  assert.equal(deviceNameFromMac("20:6e:f1:ba:65:50", "A".repeat(23)), "A".repeat(23) + "BA6550");
  assert.equal(deviceNameFromMac("20:6e:f1:ba:65:50", ""), "BA6550");
  for (const bad of ["A".repeat(24), "LAMP ", "LAMP.", "LÁMPA-"]) {
    assert.throws(() => deviceNameFromMac("20:6e:f1:ba:65:50", bad), /prefix/, JSON.stringify(bad));
  }
  validateName("A".repeat(29));
  for (const bad of ["", "A".repeat(30), "my lamp", "lamp#1"]) {
    assert.throws(() => validateName(bad), /name/, JSON.stringify(bad));
  }
  assert.throws(() => setupUrl({ base: "https://pairble.com/s", name: "my lamp", pop: "abcd1234" }), /name/);
});

test("generated PoPs use the unambiguous alphabet and do not repeat", () => {
  const seen = new Set<string>();
  for (let i = 0; i < 2000; i++) {
    const pop = generatePop();
    assert.match(pop, /^[0-9A-HJKMNP-TV-Z]{12}$/, "no I, L, O or U");
    seen.add(pop);
  }
  assert.equal(seen.size, 2000);
});

test("PoPs that would break the URL, the CSV or the firmware buffer are refused", () => {
  // 64 would be taken for a raw hex key by the SoftAP, which uses the PoP as
  // its WPA2 password.
  for (const bad of ["", "short77", "a,bcdefgh", "a bcdefgh", "line\nbreak", "x".repeat(64), "žžžžžžžž"]) {
    assert.throws(() => validatePop(bad), Error, JSON.stringify(bad));
  }
  validatePop("abcd1234");
  validatePop("x".repeat(63));
  assert.throws(() => generatePop(64), /8-63/);
});

test("setup URL carries name and pop in the fragment, never the query", () => {
  const raw = setupUrl({ base: "https://pairble.com/s", name: "PROV_BA6550", pop: "abcd1234" });
  assert.equal(raw, "https://pairble.com/s#name=PROV_BA6550&pop=abcd1234");
  const url = new URL(raw);
  assert.equal(url.search, "", "nothing a server would log");
  const params = new URLSearchParams(url.hash.slice(1));
  assert.equal(params.get("name"), "PROV_BA6550");
  assert.equal(params.get("pop"), "abcd1234");
});

test("Wi-Fi QR matches what the firmware prints for the same unit", () => {
  // From the device's own log: pairble: wifi qr: WIFI:S:PAIRBLE-BA6550;T:WPA;P:XG9S6W639Z;;
  assert.equal(wifiQr({ name: "PAIRBLE-BA6550", pop: "XG9S6W639Z" }), "WIFI:S:PAIRBLE-BA6550;T:WPA;P:XG9S6W639Z;;");
});

test("Wi-Fi QR escapes the characters the format reserves", () => {
  // Built from char codes: backslashes in test sources are too easy to lose.
  const bs = String.fromCharCode(92);
  assert.equal(
    wifiQr({ name: "a;b" + bs + "c", pop: "XG9S6W639Z" }),
    "WIFI:S:a" + bs + ";b" + bs + bs + "c;T:WPA;P:XG9S6W639Z;;",
  );
});

test("NVS CSV puts the PoP where the firmware reads it", () => {
  assert.equal(nvsCsv("K3F9Q2M7ZX"), "key,type,encoding,value\npairble,namespace,,\npop,data,string,K3F9Q2M7ZX\n");
});
