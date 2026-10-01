// SPDX-License-Identifier: Apache-2.0
// Source language. Terms and tone for every language: docs/i18n-glossary.md.

/** CLDR plural categories, as `Intl.PluralRules` returns them. `other` is always required. */
export type Plural = { other: string } & Partial<Record<"zero" | "one" | "two" | "few" | "many", string>>;

export const en = {
  pageTitle: "Set up Wi‑Fi",
  deviceFallback: "your device",
  demoBadge: "Demo, no device",

  introHeading: "Connect this device to your Wi‑Fi",
  introBody: "Your browser will ask which device to use. Choose {name}.",
  introBodyNoName: "Your browser will ask which device to use. Choose the one named on its label.",
  introNote: "Keep your phone close to the device.",
  introHttpHeading: "Choose the Wi‑Fi this device should use",
  introHttpBody: "Your phone is connected to the device's own network. It will switch back when you're done.",
  connect: "Connect",
  start: "Start",
  cancelled: "No device was chosen. Tap Connect and choose {name}.",
  bluetoothOff: "Bluetooth is off. Turn it on, then tap Connect again.",
  noDevice: "{name} didn't show up. Make sure it's switched on and close by, then try again.",

  connectingHeading: "Connecting to {name}",
  connectingBody: "Setting up a secure connection.",
  reconnectBody: "Choose the device again to carry on. The connection was closed while the page was idle, so other phones could find it.",
  scanningHeading: "Looking for Wi‑Fi networks",
  scanningBody: "The device is checking which networks it can reach. This takes about 5 seconds.",
  scanningBodyAp: "The device is checking which networks it can reach. This can take up to 30 seconds.",

  listHeading: "Choose your Wi‑Fi",
  listBody: "These are the networks the device can reach.",
  listEmpty: "The device didn't find any networks. Move it closer to your router and search again.",
  searchAgain: "Search again",
  otherNetwork: "Other network…",
  secured: "Needs a password",
  open: "Open network",
  enterprise: "Work login, not supported",
  signal: ["Weak signal", "Fair signal", "Good signal", "Excellent signal"] as [string, string, string, string],
  notFound: "The device couldn't find “{ssid}”. Move it closer to your router, or choose another network.",

  passwordHeading: "Password for {ssid}",
  passwordLabel: "Wi‑Fi password",
  show: "Show",
  hide: "Hide",
  join: "Join",
  chooseAnother: "Choose a different network",
  tooShort: "Wi‑Fi passwords have at least 8 characters.",
  tooLong: "Wi‑Fi passwords have at most 63 characters.",
  wrongPassword: "That password didn't work. Check it and try again.",

  otherHeading: "Join another network",
  ssidLabel: "Network name",
  passwordOptional: "Password (leave empty for an open network)",

  joiningHeading: "Joining {ssid}",
  joiningBody: "The device is trying the password. This can take up to half a minute.",
  attemptsLeft: {
    one: "Not connected yet, one more try left.",
    other: "Not connected yet, {n} more tries left.",
  } as Plural,
  seconds: "{s} s",

  successHeading: "Done, the device is online",
  successBody: "It's restarting and will join “{ssid}” on its own. You can close this page.",

  wrongCodeHeading: "This code doesn't match the device",
  wrongCodeBody: "Scan the code printed on the device itself, then try again.",
  lostHeading: "Lost the connection to the device",
  lostBody: "Make sure it's switched on and close by, then connect again.",
  connectAgain: "Connect again",
  timeoutHeading: "The device stopped answering",
  timeoutBody: "Wait a few seconds, then connect again. If it keeps happening, switch the device off and on.",
  unknownHeading: "Something went wrong",
  unknownBody: "Connect again to start over.",

  missingHeading: "Scan the code on your device",
  missingBody: "This page needs the code from the device's label. Open your phone's camera and scan it again.",

  iosHeading: "Use your camera to set up this device",
  iosBody: "Browsers on iPhone and iPad can't talk to Bluetooth devices. Open the Camera app and point it at the code on the device. A setup card will appear.",
  androidHeading: "Open this page in Chrome",
  androidWebview: "This app's built-in browser can't use Bluetooth.",
  androidFirefox: "Firefox can't use Bluetooth.",
  androidOther: "This browser can't use Bluetooth.",
  openInChrome: "Open in Chrome",
  desktopHeading: "Open this page in Chrome or Edge",
  desktopBody: "This browser can't use Bluetooth. Use Chrome or Edge, or scan the code with your phone.",
  insecureHeading: "Open the secure version of this page",
  insecureBody: "Bluetooth only works on pages that start with https://.",
  openSecure: "Open secure page",
};

export type Dict = typeof en;
