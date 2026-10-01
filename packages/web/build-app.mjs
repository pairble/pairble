// Builds the setup page into one self-contained HTML file.
//
// SPDX-License-Identifier: Apache-2.0
//
// One file, no external requests: the same artefact is hosted over HTTPS and
// flashed into the device, where there is no internet to fetch anything from.
// The inline script and style are pinned by hash in a Content-Security-Policy,
// so even a compromised host cannot inject a script that reads the proof of
// possession out of the URL.
//
// Optional environment:
//   PAIRBLE_APP_ID, PAIRBLE_APP_CLIP_BUNDLE_ID  emit the Safari App Clip banner

import { build, transform } from "esbuild";
import { createHash } from "node:crypto";
import { mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { gzipSync } from "node:zlib";
import { pageCsp } from "./csp.mjs";

const here = dirname(fileURLToPath(import.meta.url));
const out = resolve(here, "dist-app/index.html");

const js = await build({
  entryPoints: [resolve(here, "app/main.ts")],
  bundle: true,
  minify: true,
  format: "iife",
  target: ["chrome90", "safari15", "firefox100"],
  write: false,
  legalComments: "none",
  // Bundle core from source so the page never picks up a stale dist.
  alias: { "@pairble/core": resolve(here, "../core/src/index.ts") },
  logLevel: "warning",
});
const script = js.outputFiles[0].text.replace(/<\/script/gi, "<\\/script").trim();

const css = (await transform(readFileSync(resolve(here, "app/styles.css"), "utf8"), {
  loader: "css",
  minify: true,
  target: ["chrome90", "safari15"],
})).code.trim();

const sha = (text) => `'sha256-${createHash("sha256").update(text, "utf8").digest("base64")}'`;
// Shared with the hosted copy's header, see csp.mjs.
const csp = pageCsp({ scripts: [sha(script)], styles: [sha(css)] }).join("; ");

const appId = process.env.PAIRBLE_APP_ID;
const clipId = process.env.PAIRBLE_APP_CLIP_BUNDLE_ID;
const appleMeta = appId && clipId
  ? `<meta name="apple-itunes-app" content="app-id=${appId}, app-clip-bundle-id=${clipId}, app-clip-display=card">`
  : "";

const html = readFileSync(resolve(here, "app/index.html"), "utf8")
  .replace("__CSP__", csp)
  .replace("__APPLE_APP_META__", appleMeta)
  .replace("__CSS__", () => css)
  .replace("__JS__", () => script)
  .replace(/\n{2,}/g, "\n");

mkdirSync(dirname(out), { recursive: true });
writeFileSync(out, html);

const raw = Buffer.byteLength(html);
const gzipped = gzipSync(html, { level: 9 });
console.log(`dist-app/index.html  ${(raw / 1024).toFixed(1)} KB raw, ${(gzipped.length / 1024).toFixed(1)} KB gzip`);

// The same file, compressed, for the firmware to serve over its access point.
// Committed with the component so that building firmware needs no Node. Node
// writes no timestamp into the gzip header, so the bytes only change when the
// page does - which keeps it honest to check in.
const embedded = resolve(here, "../../components/pairble/web/setup.html.gz");
mkdirSync(dirname(embedded), { recursive: true });
writeFileSync(embedded, gzipped);
console.log(`components/pairble/web/setup.html.gz  ${(gzipped.length / 1024).toFixed(1)} KB`);
