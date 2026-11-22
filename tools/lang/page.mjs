// What a page loads for its texts (the dashboard from the Y1, the admin page from the server):
// {code, name, languages: [{code, name}], strings: {key: text}}, the strings English's keys in the
// language asked for, English's where it has none. For the mock servers (tools/dashboard-mock,
// tools/admin-mock): read from lang/ on every request, so an edit shows on the next reload.
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const DIR = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../../lang');
const read = (code) => {
  if (!/^[A-Za-z0-9_-]{1,16}$/.test(code)) return null;
  try { return JSON.parse(fs.readFileSync(path.join(DIR, `${code}.json`), 'utf8')); } catch { return null; }
};

export function languages() {
  return fs.readdirSync(DIR).filter((f) => f.endsWith('.json')).map((f) => f.slice(0, -5))
    .map((code) => ({ code, name: read(code)?._name || '' })).filter((l) => l.name)
    .sort((a, b) => (a.code === 'en' ? -1 : b.code === 'en' ? 1 : a.name.localeCompare(b.name)));
}

// "es-MX,es;q=0.9,en" -> the nearest language there's a file for, else English
export function pickLanguage(wanted) {
  const have = languages().map((l) => l.code);
  for (const part of String(wanted).split(',')) {
    const w = part.split(';')[0].trim().toLowerCase();
    if (!w) continue;
    const exact = have.find((c) => c.toLowerCase() === w);
    if (exact) return exact;
    const base = w.split(/[-_]/)[0];
    const near = have.find((c) => c.toLowerCase() === base) || have.find((c) => c.toLowerCase().split(/[-_]/)[0] === base);
    if (near) return near;
  }
  return 'en';
}

export function pageStrings(section, wanted) {
  const code = pickLanguage(wanted);
  const en = read('en') || {}, other = code === 'en' ? en : read(code) || {};
  const strings = {};
  for (const [k, v] of Object.entries(en[section] || {})) strings[k] = other[section]?.[k] ?? v;
  return { code, name: other._name || 'English', languages: languages(), strings };
}
