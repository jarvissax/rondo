// Rondo's languages on the server: lang/*.json at the repo's root (beside dist/ in the image), one file
// per language with a section per surface (docs/translating.md). The admin page reads "admin"; what the
// server says to a Rondo in words of its own (say()) comes from "device". English fills in what a
// language lacks.
import { AsyncLocalStorage } from "node:async_hooks";
import { existsSync, readdirSync, readFileSync, statSync } from "node:fs";
import { join } from "node:path";
import { fileURLToPath } from "node:url";

// /app/lang in the image (dist/../lang), lang/ in a checkout (src/../../lang)
const DIRS = ["../lang/", "../../lang/"].map((p) => fileURLToPath(new URL(p, import.meta.url)));
export const LANG_DIR = DIRS.find((d) => existsSync(join(d, "en.json"))) ?? DIRS[0]!;

type Text = string | Record<string, string>;
type File = { _name?: string; [section: string]: unknown };

const cache = new Map<string, { mtime: number; file: File | null }>();
function read(code: string, dir = LANG_DIR): File | null {
  if (!/^[A-Za-z0-9_-]{1,16}$/.test(code)) return null;
  const path = join(dir, `${code}.json`);
  let mtime: number;
  try {
    mtime = statSync(path).mtimeMs;
  } catch {
    return null;
  }
  const hit = cache.get(path);
  if (hit && hit.mtime === mtime) return hit.file;
  let file: File | null = null;
  try {
    file = JSON.parse(readFileSync(path, "utf8")) as File;
  } catch {
    file = null;
  }
  cache.set(path, { mtime, file });
  return file;
}

export type Language = { code: string; name: string };
export function languages(dir = LANG_DIR): Language[] {
  let names: string[] = [];
  try {
    names = readdirSync(dir).filter((f) => f.endsWith(".json"));
  } catch {
    return [{ code: "en", name: "English" }];
  }
  const out = names
    .map((f) => f.slice(0, -5))
    .map((code) => ({ code, name: read(code, dir)?._name ?? "" }))
    .filter((l) => l.name);
  return out.sort((a, b) => (a.code === "en" ? -1 : b.code === "en" ? 1 : a.name.localeCompare(b.name)));
}

// The best of the languages for what a browser asks ("es-MX,es;q=0.9,en"): the same code, else the same
// language without its region (es-MX -> es), else the first with that language, else English
export function pickLanguage(wanted: string, dir = LANG_DIR): string {
  const have = languages(dir).map((l) => l.code);
  const lower = new Map(have.map((c) => [c.toLowerCase(), c]));
  for (const part of wanted.split(",")) {
    const w = part.split(";")[0]!.trim().toLowerCase();
    if (!w) continue;
    if (lower.has(w)) return lower.get(w)!;
    const base = w.split(/[-_]/)[0]!;
    if (lower.has(base)) return lower.get(base)!;
    const near = have.find((c) => c.toLowerCase().split(/[-_]/)[0] === base);
    if (near) return near;
  }
  return "en";
}

// What a page loads: {code, name, languages, strings}, the strings English's keys in that language
export function pageStrings(section: string, wanted: string, dir = LANG_DIR) {
  const code = pickLanguage(wanted, dir);
  const en = read("en", dir);
  const other = code === "en" ? en : read(code, dir);
  const enTexts = (en?.[section] ?? {}) as Record<string, Text>;
  const texts = (other?.[section] ?? {}) as Record<string, Text>;
  const strings: Record<string, Text> = {};
  for (const [k, v] of Object.entries(enTexts)) strings[k] = texts[k] ?? v;
  return { code, name: other?._name ?? "English", languages: languages(dir), strings };
}

// --- YouTube's own words in the Rondo's language ---------------------------------------------
// A Rondo says its language in Accept-Language ("es"); older ones say nothing (English). Its
// requests run with it here, so YouTube is asked in it (langFetch) and the few words the server
// puts in itself (say()) come from lang/<code>.json's "device" section, English where it has none.

const requestLang = new AsyncLocalStorage<string>();

// "es", "pt-BR": the first language a header names, if it looks like a language code
export function langFromHeader(header: string | undefined): string {
  const first = (header ?? "").split(",")[0]!.split(";")[0]!.trim();
  return /^[A-Za-z]{2,3}(-[A-Za-z0-9]{2,8})?$/.test(first) ? first : "en";
}

export function withLang<T>(lang: string, run: () => T): T {
  return requestLang.run(lang, run);
}

export function currentLang(): string {
  return requestLang.getStore() ?? "en";
}

// a text the server makes up for a Rondo (a count, a playlist's name), blanks filled
export function say(key: string, params: Record<string, string | number> = {}, dir = LANG_DIR): string {
  const lang = currentLang();
  const pick = (file: File | null): Text | undefined => (file?.device as Record<string, Text> | undefined)?.[key];
  const base = lang.split("-")[0]!;
  const t = pick(read(lang, dir)) ?? (base !== lang ? pick(read(base, dir)) : undefined) ?? pick(read("en", dir));
  if (t === undefined) return key;
  let s: string;
  if (typeof t === "string") s = t;
  else {
    const n = params.count;
    const form = typeof n === "number" ? new Intl.PluralRules(lang).select(n) : "other";
    s = t[form] ?? t.other ?? "";
  }
  return s.replace(/\{(\w+)\}/g, (m, k: string) => (k in params ? String(params[k]) : m));
}

// youtubei.js's fetch: YouTube asked in the language of the request it serves (its sessions are
// created in English and shared by every Rondo; the context's hl is per request)
export const langFetch: typeof fetch = async (input, init) => {
  const lang = currentLang();
  if (lang !== "en" && init && typeof init.body === "string" && init.body.startsWith("{")) {
    try {
      const body = JSON.parse(init.body) as { context?: { client?: { hl?: string } } };
      if (body.context?.client) {
        body.context.client.hl = lang;
        init = { ...init, body: JSON.stringify(body) };
      }
    } catch {
      /* not JSON: as it is */
    }
  }
  return fetch(input, init);
};
