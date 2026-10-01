// SPDX-License-Identifier: Apache-2.0
import assert from "node:assert/strict";
import test from "node:test";
import { createIdleRelease } from "../app/idle.js";

/** A manual clock, so the tests do not wait two real minutes. */
function clock() {
  let now = 0;
  const timers = new Map<number, { at: number; fn: () => void }>();
  let next = 1;
  return {
    setTimer: (fn: () => void, ms: number) => { const id = next++; timers.set(id, { at: now + ms, fn }); return id; },
    clearTimer: (id: unknown) => { timers.delete(id as number); },
    advance(ms: number) {
      now += ms;
      for (const [id, t] of [...timers]) if (t.at <= now) { timers.delete(id); t.fn(); }
    },
  };
}

function setup(state: { open: boolean; busy: boolean }) {
  const c = clock();
  let released = 0;
  const idle = createIdleRelease({
    timeoutMs: 120_000,
    canRelease: () => state.open && !state.busy,
    release: () => { released++; state.open = false; },
    setTimer: c.setTimer,
    clearTimer: c.clearTimer,
  });
  return { c, idle, released: () => released };
}

test("an idle open session is released after the timeout", () => {
  const s = { open: true, busy: false };
  const { c, idle, released } = setup(s);
  idle.activity();
  c.advance(119_999);
  assert.equal(released(), 0);
  c.advance(1);
  assert.equal(released(), 1);
});

test("activity restarts the clock, so typing a long password is never cut off", () => {
  const s = { open: true, busy: false };
  const { c, idle, released } = setup(s);
  idle.activity();
  for (let i = 0; i < 10; i++) { c.advance(100_000); idle.activity(); }
  assert.equal(released(), 0);
});

test("nothing is released while an operation is in flight", () => {
  const s = { open: true, busy: true };
  const { c, idle, released } = setup(s);
  idle.activity();
  idle.hidden();
  c.advance(600_000);
  assert.equal(released(), 0, "a join in progress would lose its result");
});

test("an operation that is still running when the timer fires is not interrupted", () => {
  const s = { open: true, busy: false };
  const { c, idle, released } = setup(s);
  idle.activity();
  s.busy = true;
  c.advance(120_000);
  assert.equal(released(), 0);
});

test("hiding the page releases at once when nothing is running", () => {
  const s = { open: true, busy: false };
  const { idle, released } = setup(s);
  idle.hidden();
  assert.equal(released(), 1);
});

test("with no session open there is nothing to release", () => {
  const s = { open: false, busy: false };
  const { c, idle, released } = setup(s);
  idle.activity();
  idle.hidden();
  c.advance(600_000);
  assert.equal(released(), 0);
});
