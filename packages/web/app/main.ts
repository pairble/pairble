/**
 * The Pairble setup page: one column, one decision per screen.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Rendering is plain DOM - each screen is rebuilt from its state, focus moves
 * to its heading, and anything worth hearing goes through one polite live
 * region. No framework: this file ships inside the device's flash too.
 */

import { connectOverBle } from "../src/index.js";
import { fetchDeviceName, localConnect } from "./local.js";
import type { Client, Connect, JoinProgress } from "./client.js";
import { createIdleRelease } from "./idle.js";
import { demoConnect, demoEnv } from "./demo.js";
import { chromeIntentUrl, detectEnvironment, readEnvironment, secureUrl, type Env } from "./env.js";
import { classifyError } from "./errors.js";
import { pickLanguage, translator, type Key } from "./i18n.js";
import { passwordProblem, prepareNetworks, ssidProblem, type NetworkRow } from "./networks.js";
import { parseParams } from "./params.js";

// ------------------------------------------------------------------ setup ---

const params = parseParams(location.search, location.hash);
const browserLanguages = navigator.languages?.length ? navigator.languages : [navigator.language];
const tr = translator(pickLanguage(params.demo && params.demoLang ? [params.demoLang] : browserLanguages));
const t = tr.t;
document.documentElement.lang = tr.lang;
document.title = t("pageTitle");

const env: Env = params.demo ? demoEnv(params.demoEnv) : detectEnvironment(readEnvironment());
let deviceName = params.name ?? (params.demo ? "PAIRBLE-BA6550" : null);
let nameText = deviceName ?? t("deviceFallback");

/** How long to wait for a verdict. Two attempts report in ~20 s, older
 *  firmware with five in ~54 s; this covers both with room to spare. */
const JOIN_TIMEOUT_MS = 90_000;

function makeConnect(): Connect | null {
  if (params.demo) return env.kind === "ble" || env.kind === "http" ? demoConnect(params.fast) : null;
  // Served by the device over its own access point: no proof of possession in
  // the URL, and none needed - joining the WPA2 network took it.
  if (env.kind === "http") return localConnect();
  if (params.pop === null) return null;
  const pop = params.pop;
  if (env.kind === "ble") {
    return () => {
      let session: Client | null = null;
      // connectOverBle is called synchronously so requestDevice() keeps the
      // click's user gesture.
      const opening = connectOverBle({
        pop,
        ...(deviceName !== null ? { namePrefix: deviceName } : {}),
        ...(params.serviceUuid !== null ? { serviceUuid: params.serviceUuid } : {}),
        onDisconnect: () => deviceGone(session),
      });
      return opening.then((s) => (session = s));
    };
  }
  return null;
}
const connect = makeConnect();

// ------------------------------------------------------------------- dom ---

type Child = Node | string | null | false | undefined;

function h<K extends keyof HTMLElementTagNameMap>(
  tag: K,
  props: Record<string, unknown> = {},
  ...children: Child[]
): HTMLElementTagNameMap[K] {
  const el = document.createElement(tag);
  for (const [key, value] of Object.entries(props)) {
    if (value === undefined || value === null || value === false) continue;
    if (key.startsWith("on") && typeof value === "function") {
      el.addEventListener(key.slice(2).toLowerCase(), value as EventListener);
    } else if (key === "class") {
      el.className = String(value);
    } else if (value === true) {
      el.setAttribute(key, "");
    } else {
      el.setAttribute(key, String(value));
    }
  }
  for (const child of children) {
    if (child === null || child === false || child === undefined) continue;
    el.append(child);
  }
  return el;
}

const SVG = "http://www.w3.org/2000/svg";

function bars(level: 1 | 2 | 3 | 4, label: string): SVGSVGElement {
  const svg = document.createElementNS(SVG, "svg");
  svg.setAttribute("viewBox", "0 0 20 16");
  svg.setAttribute("class", "bars");
  svg.setAttribute("role", "img");
  svg.setAttribute("aria-label", label);
  for (let i = 0; i < 4; i++) {
    const rect = document.createElementNS(SVG, "rect");
    const height = 4 + i * 4;
    rect.setAttribute("x", String(i * 5 + 1));
    rect.setAttribute("y", String(16 - height));
    rect.setAttribute("width", "3");
    rect.setAttribute("height", String(height));
    rect.setAttribute("rx", "1");
    if (i < level) rect.setAttribute("class", "on");
    svg.append(rect);
  }
  return svg;
}

function lock(label: string): SVGSVGElement {
  const svg = document.createElementNS(SVG, "svg");
  svg.setAttribute("viewBox", "0 0 12 16");
  svg.setAttribute("class", "lock");
  svg.setAttribute("role", "img");
  svg.setAttribute("aria-label", label);
  const body = document.createElementNS(SVG, "rect");
  body.setAttribute("x", "1");
  body.setAttribute("y", "7");
  body.setAttribute("width", "10");
  body.setAttribute("height", "8");
  body.setAttribute("rx", "2");
  const shackle = document.createElementNS(SVG, "path");
  shackle.setAttribute("d", "M3.5 7V4.8a2.5 2.5 0 0 1 5 0V7");
  svg.append(body, shackle);
  return svg;
}

const stepEl = document.getElementById("step") as HTMLElement;
const liveEl = document.getElementById("live") as HTMLElement;
const ledEl = document.getElementById("led") as HTMLElement;
const nameEl = document.getElementById("device-name") as HTMLElement;

nameEl.textContent = nameText;
if (deviceName === null) nameEl.classList.add("is-generic");
if (params.demo) (document.getElementById("demo") as HTMLElement).hidden = false;
(document.getElementById("demo") as HTMLElement).textContent = t("demoBadge");

function announce(text: string): void {
  // Clearing first makes screen readers repeat an identical message.
  liveEl.textContent = "";
  // A timeout rather than requestAnimationFrame, which does not run in a
  // backgrounded tab - and people do switch away during a half-minute wait.
  setTimeout(() => (liveEl.textContent = text), 60);
}

type Led = "idle" | "waiting" | "busy" | "linked" | "online" | "error";

let cleanup: (() => void) | null = null;
/** The first screen is where the page starts anyway, so focus is left alone;
 *  moving it on load only makes some browsers draw a ring around the heading
 *  before the user has touched anything. */
let firstMount = true;

function mount(led: Led, heading: string, ...content: Child[]): void {
  cleanup?.();
  cleanup = null;
  ledEl.dataset.state = led;
  const title = h("h1", { tabindex: "-1" }, heading);
  stepEl.replaceChildren(title, ...content.filter((c): c is Node | string => !!c));
  // On later steps the heading takes focus so a screen reader announces where
  // the user now is. It is not a control, so it never shows a focus ring.
  if (!firstMount) title.focus({ preventScroll: true });
  firstMount = false;
  window.scrollTo(0, 0);
}

const p = (text: string, cls = "") => h("p", cls ? { class: cls } : {}, text);

/** A paragraph where the device name is set in the label's monospace, so it
 *  reads as the same code the person will look for in the browser's list. */
function pName(key: Key): HTMLParagraphElement {
  const [before = "", after = ""] = t(key, { name: "\u0000" }).split("\u0000");
  return h("p", {}, before, h("span", { class: "mono" }, nameText), after);
}
const alertBox = (text: string | undefined) => (text ? h("p", { class: "notice", role: "alert" }, text) : null);
const primary = (label: string, onclick: () => void) =>
  h("button", { class: "button primary", type: "button", onclick }, label);
const quiet = (label: string, onclick: () => void) =>
  h("button", { class: "button quiet", type: "button", onclick }, label);
const linkButton = (label: string, href: string) => h("a", { class: "button primary", href }, label);

// --------------------------------------------------------------- screens ---

let client: Client | null = null;
let networks: NetworkRow[] = [];

function showMissing(): void {
  mount("idle", t("missingHeading"), p(t("missingBody")));
}

function showUnsupported(e: Env): void {
  switch (e.kind) {
    case "ios":
      mount("idle", t("iosHeading"), p(t("iosBody")));
      break;
    case "android": {
      const why: Key =
        e.browser === "webview" ? "androidWebview" : e.browser === "firefox" ? "androidFirefox" : "androidOther";
      mount("idle", t("androidHeading"), p(t(why)), linkButton(t("openInChrome"), chromeIntentUrl(location.href)));
      break;
    }
    case "insecure":
      mount("idle", t("insecureHeading"), p(t("insecureBody")), linkButton(t("openSecure"), secureUrl(location.href)));
      break;
    default:
      mount("idle", t("desktopHeading"), p(t("desktopBody")));
  }
}

function showIntro(notice?: string): void {
  if (env.kind === "http") {
    mount("waiting", t("introHttpHeading"), alertBox(notice), p(t("introHttpBody")), primary(t("start"), () => void run()));
    return;
  }
  mount(
    "waiting",
    t("introHeading"),
    alertBox(notice),
    deviceName !== null ? pName("introBody") : p(t("introBodyNoName")),
    primary(t("connect"), () => void run()),
    p(t("introNote"), "hint"),
  );
}

function showList(notice?: string): void {
  if (networks.length === 0) {
    mount("linked", t("listHeading"), alertBox(notice), p(t("listEmpty")), primary(t("searchAgain"), () => void scan()),
      quiet(t("otherNetwork"), () => showOther()));
    return;
  }
  const rows = networks.map((n) => {
    const meta = h("span", { class: "net-meta" },
      n.secured ? lock(t("secured")) : h("span", { class: "visually-hidden" }, t("open")),
      bars(n.bars, tr.signal(n.bars)));
    const label = h("span", { class: "net-name" }, n.ssid);
    if (!n.supported) {
      return h("li", {}, h("button", { class: "net", type: "button", disabled: true },
        h("span", { class: "net-text" }, label, h("span", { class: "net-note" }, t("enterprise"))), meta));
    }
    return h("li", {}, h("button", { class: "net", type: "button", onclick: () => choose(n) }, label, meta));
  });
  rows.push(h("li", {}, h("button", { class: "net net-other", type: "button", onclick: () => showOther() },
    h("span", { class: "net-name" }, t("otherNetwork")))));

  mount("linked", t("listHeading"), alertBox(notice), p(t("listBody"), "lede"),
    h("ul", { class: "networks" }, ...rows),
    quiet(t("searchAgain"), () => void scan()));
}

function choose(n: NetworkRow): void {
  if (!n.secured) {
    void join(n.ssid, "", n);
    return;
  }
  showPassword(n);
}

function passwordField(id: string, labelText: string): { wrap: HTMLElement; input: HTMLInputElement } {
  const input = h("input", {
    id, type: "password", class: "input", autocomplete: "off", autocapitalize: "none",
    autocorrect: "off", spellcheck: "false", enterkeyhint: "go",
  });
  const toggle = h("button", { class: "reveal", type: "button", "aria-controls": id, "aria-pressed": "false" }, t("show"));
  toggle.addEventListener("click", () => {
    const hidden = input.type === "password";
    input.type = hidden ? "text" : "password";
    toggle.textContent = hidden ? t("hide") : t("show");
    toggle.setAttribute("aria-pressed", String(hidden));
    input.focus();
  });
  const wrap = h("div", { class: "field" }, h("label", { for: id }, labelText), h("div", { class: "input-wrap" }, input, toggle));
  return { wrap, input };
}

function showPassword(n: NetworkRow, notice?: string, previous = ""): void {
  const { wrap, input } = passwordField("password", t("passwordLabel"));
  // After a wrong password, keep what was typed: it is usually one wrong
  // character, and retyping twenty to fix one is the wrong kind of friction.
  input.value = previous;
  const problemEl = h("p", { class: "field-error", id: "password-problem", role: "alert" });
  input.setAttribute("aria-describedby", "password-problem");
  const form = h("form", { class: "form", novalidate: true }, wrap, problemEl, h("button", { class: "button primary", type: "submit" }, t("join")));
  form.addEventListener("submit", (event) => {
    event.preventDefault();
    const problem = passwordProblem(n.auth, input.value);
    if (problem) {
      problemEl.textContent = t(problem === "too-short" ? "tooShort" : "tooLong");
      input.setAttribute("aria-invalid", "true");
      input.focus();
      return;
    }
    void join(n.ssid, input.value, n);
  });

  mount("linked", t("passwordHeading", { ssid: n.ssid }), alertBox(notice), form,
    quiet(t("chooseAnother"), () => showList()));
}

function showOther(notice?: string, ssid = "", previous = ""): void {
  const ssidInput = h("input", { id: "ssid", class: "input", autocomplete: "off", autocapitalize: "none",
    autocorrect: "off", spellcheck: "false", value: ssid, maxlength: "32" });
  const { wrap, input } = passwordField("other-password", t("passwordOptional"));
  input.value = previous;
  const problemEl = h("p", { class: "field-error", id: "other-password-problem", role: "alert" });
  input.setAttribute("aria-describedby", "other-password-problem");
  const form = h("form", { class: "form", novalidate: true },
    h("div", { class: "field" }, h("label", { for: "ssid" }, t("ssidLabel")), ssidInput),
    wrap, problemEl,
    h("button", { class: "button primary", type: "submit" }, t("join")));
  form.addEventListener("submit", (event) => {
    event.preventDefault();
    // Not trimmed: a space is a legal part of an SSID, and a name missing it
    // is a different network.
    const name = ssidInput.value;
    // maxlength counts UTF-16 units, the limit is 32 bytes; nothing longer
    // can be a real network, so the field is flagged rather than explained.
    if (ssidProblem(name)) {
      ssidInput.setAttribute("aria-invalid", "true");
      ssidInput.focus();
      return;
    }
    ssidInput.removeAttribute("aria-invalid");
    const problem = passwordProblem(null, input.value);
    if (problem) {
      problemEl.textContent = t(problem === "too-short" ? "tooShort" : "tooLong");
      input.setAttribute("aria-invalid", "true");
      input.focus();
      return;
    }
    void join(name, input.value, null);
  });
  mount("linked", t("otherHeading"), alertBox(notice), form,
    networks.length > 0 ? quiet(t("chooseAnother"), () => showList()) : null);
}

function showJoining(ssid: string): (progress: JoinProgress) => void {
  const value = h("span", { class: "timer-value" }, "0");
  const timer = h("p", { class: "timer", "aria-hidden": "true" }, value, h("span", { class: "timer-unit" }, "s"));
  const attempts = h("p", { class: "hint attempts" });
  mount("busy", t("joiningHeading", { ssid }), timer, p(t("joiningBody")), attempts);
  announce(t("joiningBody"));

  const started = Date.now();
  const tick = setInterval(() => {
    value.textContent = String(Math.floor((Date.now() - started) / 1000));
  }, 250);
  cleanup = () => clearInterval(tick);

  let lastCount: number | undefined;
  return (progress) => {
    const n = progress.attemptsRemaining;
    if (n === undefined || n === lastCount) return;
    lastCount = n;
    attempts.textContent = tr.attemptsLeft(n);
    announce(attempts.textContent);
  };
}

function showFailure(heading: Key, body: Key): void {
  mount("error", t(heading), p(t(body)), primary(t("connectAgain"), () => void run()));
}

// ------------------------------------------------------------------ flow ---

async function dispose(): Promise<void> {
  idle.cancel();
  const c = client;
  client = null;
  await c?.close().catch(() => {});
}

/** Operations in flight. The session is never released under one. */
let inFlight = 0;

async function operation<T>(fn: () => Promise<T>): Promise<T> {
  inFlight++;
  try {
    return await fn();
  } finally {
    inFlight--;
    idle.activity();
  }
}

// A connected phone stops the device advertising, so an abandoned page must not
// sit on the connection (idle.ts has the measurement that prompted this).
const idle = createIdleRelease({
  timeoutMs: 120_000,
  canRelease: () => client !== null && inFlight === 0,
  release: () => void dispose(),
});
for (const type of ["pointerdown", "keydown", "input"] as const) {
  document.addEventListener(type, () => idle.activity(), { passive: true });
}
document.addEventListener("visibilitychange", () => {
  if (document.visibilityState === "hidden") idle.hidden();
});

/**
 * The device dropped the link on its own - most often the firmware's idle
 * guard, which disconnects a phone that holds the device too long. If nothing
 * is running, forget the session quietly: the next action reconnects, exactly
 * as after the page's own idle release. If something is running, it fails and
 * reports the loss itself. A late event from an older session is ignored, so
 * it cannot knock out a newer one.
 */
function deviceGone(session: Client | null): void {
  if (session === null || session !== client || inFlight > 0) return;
  idle.cancel();
  client = null;
}

/**
 * Reopens a session released while the page was idle. Called straight from a
 * click, so the chooser still has its user gesture: nothing here awaits before
 * connect() reaches requestDevice().
 */
async function reconnect(): Promise<boolean> {
  if (!connect) return false;
  mount("busy", t("connectingHeading", { name: nameText }), p(t("reconnectBody")));
  try {
    client = await operation(connect);
    return true;
  } catch (error) {
    fail(error);
    return false;
  }
}

function fail(error: unknown): void {
  const outcome = classifyError(error);
  // Name and outcome only. Nothing here may carry the PoP or a password, and
  // core's messages never do, but there is no reason to find out.
  console.warn("[pairble]", outcome, (error as Error)?.name);
  void dispose();
  switch (outcome) {
    case "cancelled":
      return showIntro(t("cancelled", { name: nameText }));
    case "bluetooth-off":
      return showIntro(t("bluetoothOff"));
    case "no-device":
      return showIntro(t("noDevice", { name: nameText }));
    case "wrong-code":
      return showFailure("wrongCodeHeading", "wrongCodeBody");
    case "lost":
      return showFailure("lostHeading", "lostBody");
    case "timeout":
      return showFailure("timeoutHeading", "timeoutBody");
    default:
      return showFailure("unknownHeading", "unknownBody");
  }
}

async function run(): Promise<void> {
  if (!connect) return;
  mount("busy", t("connectingHeading", { name: nameText }), p(t("connectingBody")));
  try {
    client = await operation(connect);
  } catch (error) {
    fail(error);
    return;
  }
  await scan();
}

async function scan(): Promise<void> {
  if (!client) return run();
  const track = h("div", { class: "track", "aria-hidden": "true" });
  // Demo at five times speed shortens the track to match. Through CSSOM: the
  // page's CSP refuses style attributes.
  if (params.demo && params.fast) track.style.setProperty("--duration", "1.1s");
  // Over its own access point the device scans far slower than over BLE: one
  // radio, which keeps returning to serve the access point and Bluetooth
  // between channels. Said so up front, and the bar paced to match.
  const overAp = env.kind === "http";
  if (overAp && !params.fast) track.style.setProperty("--duration", "25s");
  mount("busy", t("scanningHeading"), p(t(overAp ? "scanningBodyAp" : "scanningBody")), track);
  announce(t("scanningHeading"));
  const session = client;
  try {
    networks = prepareNetworks(await operation(() => session.scan()));
  } catch (error) {
    fail(error);
    return;
  }
  showList();
}

async function join(ssid: string, password: string, network: NetworkRow | null): Promise<void> {
  // Released while the person was typing: reconnect and carry on with what
  // they entered, rather than sending them back to the start.
  if (!client && !(await reconnect())) return;
  const session = client;
  if (!session) return;
  const onProgress = showJoining(ssid);
  try {
    await operation(async () => {
      await session.applyCredentials(ssid, password);
      await session.waitForResult(onProgress, JOIN_TIMEOUT_MS);
    });
  } catch (error) {
    const outcome = classifyError(error);
    // The device resets its state machine after a failure, so the session
    // stays usable: send the person back one step, not to the start.
    if (outcome === "wrong-password") {
      return network ? showPassword(network, t("wrongPassword"), password) : showOther(t("wrongPassword"), ssid, password);
    }
    if (outcome === "network-not-found") {
      return network ? showList(t("notFound", { ssid })) : showOther(t("notFound", { ssid }), ssid, password);
    }
    fail(error);
    return;
  }
  mount("online", t("successHeading"), p(t("successBody", { ssid })));
  announce(t("successHeading"));
  void dispose();
}

// ----------------------------------------------------------------- start ---

if (env.kind === "http" && !params.demo) {
  // The device knows its own name; the URL carries none here.
  void fetchDeviceName().then((name) => {
    if (name === null) return;
    deviceName = name;
    nameText = name;
    nameEl.textContent = name;
    nameEl.classList.remove("is-generic");
  });
  showIntro();
} else if (!params.demo && params.pop === null) {
  // Unsupported browsers get their own advice first: rescanning the code in
  // the same browser would not help them.
  if (env.kind === "ble" || env.kind === "http") showMissing();
  else showUnsupported(env);
} else if (connect === null) {
  showUnsupported(env);
} else {
  showIntro();
}
