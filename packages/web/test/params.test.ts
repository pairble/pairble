// SPDX-License-Identifier: Apache-2.0
import assert from "node:assert/strict";
import test from "node:test";
import { parseParams } from "../app/params.js";

test("reads name and pop from the query, as older codes and the Chrome hand-off carry them", () => {
  const p = parseParams("?name=PROV_BA6550&pop=abcd1234", "");
  assert.equal(p.name, "PROV_BA6550");
  assert.equal(p.pop, "abcd1234");
  assert.equal(p.serviceUuid, null);
  assert.equal(p.demo, false);
});

test("the fragment wins over the query", () => {
  const p = parseParams("?name=FROM_QUERY&pop=q", "#pop=from-fragment");
  assert.equal(p.pop, "from-fragment");
  assert.equal(p.name, "FROM_QUERY");
});

test("missing or blank values come back as null, never as empty strings", () => {
  const p = parseParams("?name=%20%20&pop=", "");
  assert.equal(p.name, null);
  assert.equal(p.pop, null);
});

test("control characters are stripped and lengths capped", () => {
  const p = parseParams(`?name=${encodeURIComponent("PROV\n_X\u0000")}&pop=${"a".repeat(100)}`, "");
  assert.equal(p.name, "PROV_X");
  assert.equal(p.pop?.length, 63);
});

test("a service UUID is accepted only in canonical form", () => {
  assert.equal(parseParams("?svc=021A9004-0382-4AEA-BFF4-6B3F1C5ADFB4", "").serviceUuid,
               "021a9004-0382-4aea-bff4-6b3f1c5adfb4");
  assert.equal(parseParams("?svc=not-a-uuid", "").serviceUuid, null);
});

test("demo and fast flags", () => {
  const p = parseParams("?demo=1&fast=1", "");
  assert.equal(p.demo, true);
  assert.equal(p.fast, true);
  assert.equal(parseParams("?demo=true", "").demo, false);
});
