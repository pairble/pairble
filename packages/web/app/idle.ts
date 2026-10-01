/**
 * Lets go of the device when nobody is using the page.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * While a phone holds the BLE connection the ESP32 stops advertising, so no
 * other phone can find it. Measured on hardware: a tab left on a failure
 * screen kept a device invisible for 22 minutes, and an iPhone tried and
 * failed to find it in the meantime. The person who
 * gave up on one phone and picked up another is the one this hurts.
 *
 * Tab visibility alone would not have caught it - that tab was visible, just
 * abandoned - so an idle timer does the main work and visibility is a
 * shortcut. Nothing is released mid-operation: a join in flight would lose its
 * result, and it ends by itself within the join timeout anyway.
 */

export interface IdleReleaseOptions {
  timeoutMs: number;
  /** True when a session is open and no operation is in flight. */
  canRelease: () => boolean;
  release: () => void;
  setTimer?: (fn: () => void, ms: number) => unknown;
  clearTimer?: (handle: unknown) => void;
}

export interface IdleRelease {
  /** Call on any user activity, and whenever an operation finishes. */
  activity(): void;
  /** Call when the page is hidden. */
  hidden(): void;
  /** Stop the timer, e.g. once the session is closed for another reason. */
  cancel(): void;
}

export function createIdleRelease(opts: IdleReleaseOptions): IdleRelease {
  const setTimer = opts.setTimer ?? ((fn, ms) => setTimeout(fn, ms));
  const clearTimer = opts.clearTimer ?? ((h) => clearTimeout(h as ReturnType<typeof setTimeout>));
  let handle: unknown;

  const cancel = () => {
    if (handle !== undefined) clearTimer(handle);
    handle = undefined;
  };
  const fire = () => {
    handle = undefined;
    if (opts.canRelease()) opts.release();
  };

  return {
    activity() {
      cancel();
      if (opts.canRelease()) handle = setTimer(fire, opts.timeoutMs);
    },
    hidden() {
      cancel();
      if (opts.canRelease()) opts.release();
    },
    cancel,
  };
}
