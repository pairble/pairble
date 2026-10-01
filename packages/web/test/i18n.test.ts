// SPDX-License-Identifier: Apache-2.0
import assert from "node:assert/strict";
import test from "node:test";
import { pickLanguage, translator, type Lang } from "../app/i18n.js";
import { en } from "../app/lang/en.js";

const ALL: Lang[] = ["en", "cs", "sk", "de", "pl", "fr", "es", "it", "pt", "nl"];

test("language follows the browser, falling back to English", () => {
  assert.equal(pickLanguage(["cs-CZ", "en"]), "cs");
  assert.equal(pickLanguage(["de-DE", "en-GB"]), "de");
  assert.equal(pickLanguage(["pt-PT"]), "pt");
  assert.equal(pickLanguage(["ja-JP", "sk"]), "sk");
  assert.equal(pickLanguage(["ja-JP"]), "en");
  assert.equal(pickLanguage(["constructor"]), "en");
  assert.equal(pickLanguage([]), "en");
});

test("placeholders are filled", () => {
  assert.equal(translator("en").t("introBody", { name: "PROV_1" }),
    "Your browser will ask which device to use. Choose PROV_1.");
});

test("Czech plurals take all three forms", () => {
  const cs = translator("cs");
  assert.match(cs.attemptsLeft(1), /jeden pokus/);
  assert.match(cs.attemptsLeft(3), /zbývají ještě 3 pokusy/);
  assert.match(cs.attemptsLeft(5), /zbývá ještě 5 pokusů/);
});

test("Polish plurals take one, few and many", () => {
  const pl = translator("pl");
  assert.match(pl.attemptsLeft(1), /jedna próba/);
  assert.match(pl.attemptsLeft(2), /zostały 2 próby/);
  assert.match(pl.attemptsLeft(5), /zostało 5 prób/);
  assert.match(pl.attemptsLeft(22), /zostały 22 próby/);
});

test("every language keeps the placeholders of English", () => {
  const placeholders = (s: string) => (s.match(/\{\w+\}/g) ?? []).sort().join();
  for (const lang of ALL) {
    const tr = translator(lang);
    for (const key of Object.keys(en) as (keyof typeof en)[]) {
      const source = en[key];
      if (typeof source !== "string") continue;
      assert.equal(placeholders(tr.t(key as never, {})), "", `${lang}.${key} left a placeholder`);
      const filled = tr.t(key as never, { name: "N", ssid: "S", s: 1, n: 2 });
      assert.ok(!/\{\w+\}/.test(filled), `${lang}.${key}`);
    }
    for (const n of [1, 2, 3, 5]) assert.ok(tr.attemptsLeft(n).includes(n === 1 ? "" : String(n)), `${lang} ${n}`);
    for (const bars of [1, 2, 3, 4] as const) assert.ok(tr.signal(bars).length > 0);
  }
});

test("Wi-Fi keeps its non-breaking hyphen and French its no-break spaces", () => {
  for (const lang of ALL) {
    for (const key of Object.keys(en) as (keyof typeof en)[]) {
      if (typeof en[key] !== "string") continue;
      const text = translator(lang).t(key as never, { ssid: "S", name: "N", s: 1 });
      assert.ok(!text.includes("Wi-Fi"), `${lang}.${key}: plain hyphen`);
      if (lang === "fr") assert.ok(!/« | »/.test(text), `fr.${key}: breaking space by a guillemet`);
    }
  }
  assert.ok(translator("cs").t("pageTitle").includes("Wi‑Fi"));
});
