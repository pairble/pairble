// The setup page's Content-Security-Policy, in one place.
//
// SPDX-License-Identifier: Apache-2.0
//
// Two copies of the page enforce it: the file itself, in a <meta> tag
// (build-app.mjs), which is all the device's own web server offers; and the
// hosted copy's response header (pairble.com's build). Both build from this list
// so they cannot drift apart - a directive added to one only would be a
// policy that holds on one path and not on the other.

/**
 * @param {{ scripts: string[], styles: string[] }} hashes  quoted CSP sources,
 *   e.g. 'sha256-...', for the page's inline <script> and <style> elements
 * @returns {string[]} directives, to be joined with "; "
 */
export function pageCsp({ scripts, styles }) {
  return [
    "default-src 'none'",
    `script-src ${scripts.join(" ") || "'none'"}`,
    `style-src ${styles.join(" ") || "'none'"}`,
    // The HTTP transport talks to the device that served the page; nothing
    // else. Over BLE the page makes no requests at all - Bluetooth is not
    // governed by CSP.
    "connect-src 'self'",
    // Icons are inline SVG built in the DOM, which img-src does not cover.
    "img-src 'none'",
    "base-uri 'none'",
    "form-action 'none'",
  ];
}

/** Only honoured in a response header - a <meta> policy ignores it, and
 *  browsers warn - so the hosted copy adds it on top. */
export const HEADER_ONLY = ["frame-ancestors 'none'"];
