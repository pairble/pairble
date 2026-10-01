// SPDX-License-Identifier: Apache-2.0
import assert from "node:assert/strict";
import test from "node:test";
import { Auth, passwordProblem, prepareNetworks, signalBars, ssidProblem } from "../app/networks.js";

test("duplicates collapse to the strongest, hidden networks drop, strongest first", () => {
  const rows = prepareNetworks([
    { ssid: "Home", rssi: -80, auth: Auth.WPA2_PSK },
    { ssid: "Cafe", rssi: -60, auth: Auth.Open },
    { ssid: "", rssi: -40, auth: Auth.WPA2_PSK },
    { ssid: "Home", rssi: -55, auth: Auth.WPA2_PSK },
    { ssid: "   ", rssi: -45, auth: Auth.WPA2_PSK },
  ]);
  assert.deepEqual(rows.map((r) => [r.ssid, r.rssi]), [["Home", -55], ["Cafe", -60]]);
  assert.equal(rows[0]!.secured, true);
  assert.equal(rows[1]!.secured, false);
});

test("equal signal sorts by name so the list does not reshuffle", () => {
  const rows = prepareNetworks([
    { ssid: "b", rssi: -70, auth: Auth.WPA2_PSK },
    { ssid: "a", rssi: -70, auth: Auth.WPA2_PSK },
  ]);
  assert.deepEqual(rows.map((r) => r.ssid), ["a", "b"]);
});

test("enterprise networks are listed but marked unsupported", () => {
  // ESP-IDF's wifi_auth_mode_t: 5, 10, 14, 15 and 16 are all 802.1X.
  for (const auth of [5, 10, 14, 15, 16]) {
    const [row] = prepareNetworks([{ ssid: "Corp", rssi: -60, auth }]);
    assert.equal(row!.supported, false, `auth ${auth}`);
  }
});

test("each auth mode asks for a password, or not, as it should", () => {
  const row = (auth: number) => prepareNetworks([{ ssid: "n", rssi: -60, auth }])[0]!;
  assert.equal(row(Auth.Open).secured, false);
  assert.equal(row(Auth.OWE).secured, false, "OWE encrypts, but takes no password");
  assert.equal(row(Auth.OWE).supported, true);
  for (const auth of [Auth.WPA3_Ext_PSK, Auth.WPA3_Ext_PSK_Mixed]) {
    assert.equal(row(auth).secured, true);
    assert.equal(row(auth).supported, true);
    assert.equal(passwordProblem(auth, "short"), "too-short", "a PSK like any other");
  }
  assert.equal(row(42).secured, true, "an unknown mode is assumed to want a password");
  assert.equal(row(42).supported, true);
  assert.equal(passwordProblem(42, "x"), null, "but its length is not guessed at");
});

test("signal bars", () => {
  assert.deepEqual([-40, -55, -56, -67, -68, -78, -79, -95].map(signalBars), [4, 4, 3, 3, 2, 2, 1, 1]);
});

test("WPA passphrase length is checked before the device spends twenty seconds on it", () => {
  assert.equal(passwordProblem(Auth.WPA2_PSK, "short"), "too-short");
  assert.equal(passwordProblem(Auth.WPA2_PSK, "12345678"), null);
  assert.equal(passwordProblem(Auth.WPA2_PSK, "x".repeat(63)), null);
  assert.equal(passwordProblem(Auth.WPA3_PSK, "x".repeat(64)), "too-long");
  // Valid WPA, but the device's Bluetooth path keeps at most 63 bytes.
  assert.equal(passwordProblem(Auth.WPA2_PSK, "a".repeat(64)), "too-long", "no raw 64-hex PSK");
});

test("password limits are counted in UTF-8 bytes, not characters", () => {
  assert.equal(passwordProblem(Auth.WPA2_PSK, "ěščř"), null, "four characters, eight bytes");
  assert.equal(passwordProblem(Auth.WPA2_PSK, "ěšč"), "too-short", "three characters, six bytes");
  assert.equal(passwordProblem(Auth.WPA2_PSK, "ž".repeat(32)), "too-long", "32 characters, 64 bytes");
  assert.equal(passwordProblem(null, "ž".repeat(32)), "too-long", "no network takes 64 bytes");
});

test("a typed network name: spaces kept, 32 bytes at most", () => {
  assert.equal(ssidProblem(""), "empty");
  assert.equal(ssidProblem("   "), "empty");
  assert.equal(ssidProblem(" Home "), null, "leading and trailing spaces are part of the name");
  assert.equal(ssidProblem("x".repeat(32)), null);
  assert.equal(ssidProblem("x".repeat(33)), "too-long");
  assert.equal(ssidProblem("ř".repeat(17)), "too-long", "17 characters, 34 bytes");
});

test("no guessing where nothing is known", () => {
  assert.equal(passwordProblem(null, ""), null, "manually typed network");
  assert.equal(passwordProblem(Auth.Open, ""), null);
  assert.equal(passwordProblem(Auth.WEP, "abcde"), null);
});
