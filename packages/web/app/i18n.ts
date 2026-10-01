/**
 * Ten languages as plain dictionaries, not a library: the whole page has to fit
 * in the device's flash. Terms and tone: docs/i18n-glossary.md.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * "Wi‑Fi" uses a non-breaking hyphen (U+2011) so it never splits across lines.
 */

import { cs } from "./lang/cs.js";
import { de } from "./lang/de.js";
import { en, type Dict } from "./lang/en.js";
import { es } from "./lang/es.js";
import { fr } from "./lang/fr.js";
import { it } from "./lang/it.js";
import { nl } from "./lang/nl.js";
import { pl } from "./lang/pl.js";
import { pt } from "./lang/pt.js";
import { sk } from "./lang/sk.js";

const dictionaries = { en, cs, sk, de, pl, fr, es, it, pt, nl } satisfies Record<string, Dict>;

export type Key = { [K in keyof Dict]: Dict[K] extends string ? K : never }[keyof Dict];
export type Lang = keyof typeof dictionaries;

function isLang(tag: string): tag is Lang {
  return Object.hasOwn(dictionaries, tag);
}

export function pickLanguage(languages: readonly string[]): Lang {
  for (const tag of languages) {
    const base = tag.toLowerCase().split("-")[0] ?? "";
    if (isLang(base)) return base;
  }
  return "en";
}

export function translator(lang: Lang) {
  const dict: Dict = dictionaries[lang];
  const plural = new Intl.PluralRules(lang);
  return {
    lang,
    t(key: Key, vars: Record<string, string | number> = {}): string {
      return dict[key].replace(/\{(\w+)\}/g, (_, v: string) => String(vars[v] ?? ""));
    },
    /** Picks the CLDR plural form, e.g. Czech one/few/other, Polish one/few/many. */
    attemptsLeft(n: number): string {
      const forms = dict.attemptsLeft;
      return (forms[plural.select(n)] ?? forms.other).replace("{n}", String(n));
    },
    signal(bars: 1 | 2 | 3 | 4): string {
      return dict.signal[bars - 1]!;
    },
  };
}

export type Translator = ReturnType<typeof translator>;
