// The admin page's languages: lang/*.json at the repo's root (beside dist/ in the image), one file per
// language with a section per surface; this page reads "admin" (docs/translating.md). English fills in
// what a language lacks, so a page always has every text.
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
