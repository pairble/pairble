// SPDX-License-Identifier: Apache-2.0
import assert from "node:assert/strict";
import test from "node:test";
import { chromeIntentUrl, detectEnvironment, type EnvInput } from "../app/env.js";

const CHROME_ANDROID = "Mozilla/5.0 (Linux; Android 14; Pixel 8) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/140.0 Mobile Safari/537.36";
const WEBVIEW = "Mozilla/5.0 (Linux; Android 14; Pixel 8; wv) AppleWebKit/537.36 (KHTML, like Gecko) Version/4.0 Chrome/140.0 Mobile Safari/537.36";
const FIREFOX_ANDROID = "Mozilla/5.0 (Android 14; Mobile; rv:140.0) Gecko/140.0 Firefox/140.0";
const IPHONE = "Mozilla/5.0 (iPhone; CPU iPhone OS 26_0 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/26.0 Mobile/15E148 Safari/604.1";
const IPAD_AS_MAC = "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/26.0 Safari/605.1.15";

const base: EnvInput = {
  hasBluetooth: false, secure: true, protocol: "https:", hostname: "pairble.com", userAgent: "", maxTouchPoints: 0,
};

test("feature detection wins over the user agent", () => {
  // A desktop UA with Web Bluetooth is simply supported.
  assert.deepEqual(detectEnvironment({ ...base, hasBluetooth: true, userAgent: IPAD_AS_MAC }), { kind: "ble" });
});

test("Bluetooth outside a secure context does not count", () => {
  assert.notEqual(detectEnvironment({ ...base, hasBluetooth: true, secure: false, protocol: "http:" }).kind, "ble");
});

test("the copy served by the device talks HTTP, on any phone", () => {
  for (const hostname of ["192.168.4.1", "pairble-device.local"]) {
    assert.deepEqual(detectEnvironment({ ...base, protocol: "http:", secure: false, hostname, userAgent: IPHONE }), { kind: "http" });
  }
});

test("plain http on a real host asks for https", () => {
  assert.deepEqual(detectEnvironment({ ...base, protocol: "http:", secure: false }), { kind: "insecure" });
});

test("iPhone, and iPad pretending to be a Mac", () => {
  assert.deepEqual(detectEnvironment({ ...base, userAgent: IPHONE }), { kind: "ios" });
  assert.deepEqual(detectEnvironment({ ...base, userAgent: IPAD_AS_MAC, maxTouchPoints: 5 }), { kind: "ios" });
  assert.deepEqual(detectEnvironment({ ...base, userAgent: IPAD_AS_MAC, maxTouchPoints: 0 }), { kind: "desktop" });
});

test("Android without Bluetooth says why", () => {
  assert.deepEqual(detectEnvironment({ ...base, userAgent: WEBVIEW }), { kind: "android", browser: "webview" });
  assert.deepEqual(detectEnvironment({ ...base, userAgent: FIREFOX_ANDROID }), { kind: "android", browser: "firefox" });
  assert.deepEqual(detectEnvironment({ ...base, userAgent: CHROME_ANDROID }), { kind: "android", browser: "other" });
});

test("the Chrome intent keeps parameters, moving fragment ones into the query", () => {
  assert.equal(
    chromeIntentUrl("https://pairble.com/s?name=PROV_1#pop=abc"),
    "intent://pairble.com/s?name=PROV_1&pop=abc#Intent;scheme=https;package=com.android.chrome;end",
  );
  assert.equal(
    chromeIntentUrl("https://pairble.com/s"),
    "intent://pairble.com/s#Intent;scheme=https;package=com.android.chrome;end",
  );
});

test("localhost over plain http is a secure context and gets Bluetooth", () => {
  // The development and test-plan path: http://localhost is potentially
  // trustworthy, so Chrome exposes Web Bluetooth there. It must not be caught
  // by the "plain http on a real hostname" rule or by the device-host rule.
  const localhost = { ...base, protocol: "http:", hostname: "localhost", hasBluetooth: true, secure: true };
  assert.deepEqual(detectEnvironment(localhost), { kind: "ble" });
  assert.deepEqual(detectEnvironment({ ...localhost, hostname: "127.0.0.1" }), { kind: "ble" });
});
