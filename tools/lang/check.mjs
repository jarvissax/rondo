// Checks Rondo's languages (lang/*.json; docs/translating.md):
//   - every language has every English key, and none English doesn't have;
//   - each text has the same blanks ({count}, {name}) and the same markup (<b>, </b>) as English;
//   - plural texts have their "other" form (and English its "one");
//   - every key the code asks for is in English: tr("…") on the Y1 (native/) and say("…") in what the
//     server tells a Rondo (server/src/: its "device" section), t('…') / te('…') /
//     th('…') and data-i18n* in the dashboard (native/assets/dashboard/), the admin page (server/admin/)
//     and the update page (web-updater/: index.html and src/);
//   - the English written in those pages' HTML is the English in lang/en.json.
// Node 22, no dependencies. Exits 1 on any error.
//
//   node tools/lang/check.mjs            check
//   node tools/lang/check.mjs --format   also rewrite the files in one layout: English's keys sorted,
//                                        each language's keys in English's order
//   node tools/lang/check.mjs --unused   also list English keys no code names literally
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

export const SECTIONS = ['device', 'dashboard', 'admin', 'updater'];
const FORMS = ['zero', 'one', 'two', 'few', 'many', 'other'];
const BLANK = /\{([A-Za-z0-9_]+)\}/g;
const TAG = /<\/?[a-z][a-z0-9]*\b[^>]*>/gi;

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');

// where each surface asks for its texts
const SOURCES = {
  device: { dirs: ['native/shell', 'native/core', 'native/player', 'native/services', 'native/direct', 'native/ui', 'native/apps/rondo'], ext: ['.cpp', '.h'] },
  dashboard: { files: ['native/assets/dashboard/app.js', 'native/assets/dashboard/index.html'] },
  admin: { files: ['server/admin/admin.js', 'server/admin/index.html'] },
  updater: { files: ['web-updater/index.html'], dirs: ['web-updater/src'], ext: ['.js'] },
};
const WEB = ['dashboard', 'admin', 'updater'];

const blanks = (s) => [...s.matchAll(BLANK)].map((m) => m[1]).sort();
const tags = (s) => [...s.matchAll(TAG)].map((m) => m[0].toLowerCase().replace(/\s+/g, ' ')).sort();
const same = (a, b) => a.length === b.length && a.every((x, i) => x === b[i]);
const forms = (v) => (typeof v === 'string' ? [v] : Object.values(v));
const union = (v, f) => [...new Set(forms(v).flatMap(f))].sort();

function readJson(file, errors) {
  try {
    return JSON.parse(fs.readFileSync(file, 'utf8'));
  } catch (e) {
    errors.push(`${rel(file)}: not valid JSON (${e.message})`);
    return null;
  }
}
const rel = (f) => path.relative(ROOT, f);

function checkValue(where, v, errors, english) {
  if (typeof v === 'string') return true;
  if (!v || typeof v !== 'object' || Array.isArray(v)) {
    errors.push(`${where}: a text is a string or plural forms {"one": …, "other": …}`);
    return false;
  }
  for (const [f, s] of Object.entries(v)) {
    if (!FORMS.includes(f)) errors.push(`${where}: "${f}" isn't a plural form (${FORMS.join(', ')})`);
    else if (typeof s !== 'string') errors.push(`${where}.${f}: not a string`);
  }
  if (typeof v.other !== 'string') errors.push(`${where}: plural texts need an "other" form`);
  if (english && typeof v.one !== 'string') errors.push(`${where}: English plural texts need a "one" form`);
  return true;
}

// the keys each surface's code names
function usedKeys(root) {
  const used = Object.fromEntries(SECTIONS.map((sec) => [sec, new Map()]));
  const note = (sec, key, where) => { if (!used[sec].has(key)) used[sec].set(key, where); };
  const walk = (dir, ext, out) => {
    if (!fs.existsSync(dir)) return;
    for (const e of fs.readdirSync(dir, { withFileTypes: true })) {
      const p = path.join(dir, e.name);
      if (e.isDirectory()) walk(p, ext, out);
      else if (ext.includes(path.extname(e.name))) out.push(p);
    }
  };
  const lineOf = (text, i) => text.slice(0, i).split('\n').length;
  const files = [];
  for (const d of SOURCES.device.dirs) walk(path.join(root, d), SOURCES.device.ext, files);
  // comments out (an example in one isn't a use), keeping the lines where they are
  const uncomment = (text) => text.split('\n').map((line) => {
    for (let i = line.indexOf('//'); i >= 0; i = line.indexOf('//', i + 2))
      if ((line.slice(0, i).match(/(?<!\\)"/g) || []).length % 2 === 0) return line.slice(0, i);
    return line;
  }).join('\n');
  // the server's own words to a Rondo (server/src/lang.ts say()): the device's section too
  const serverFiles = [];
  walk(path.join(root, 'server/src'), ['.ts'], serverFiles);
  for (const f of serverFiles) {
    const text = uncomment(fs.readFileSync(f, 'utf8'));
    for (const m of text.matchAll(/\bsay\(\s*"([^"]+)"(?!\s*\+)/g)) note('device', m[1], `${path.relative(root, f)}:${lineOf(text, m.index)}`);
  }
  for (const f of files) {
    const text = uncomment(fs.readFileSync(f, 'utf8'));
    for (const m of text.matchAll(/\btr\(\s*"((?:[^"\\]|\\.)+)"(?!\s*\+)/g)) note('device', m[1], `${path.relative(root, f)}:${lineOf(text, m.index)}`);
  }
  for (const sec of WEB) {
    const { files: listed = [], dirs = [], ext = [] } = SOURCES[sec];
    const found = [];
    for (const d of dirs) walk(path.join(root, d), ext, found);
    for (const f of [...listed, ...found.sort().map((p) => path.relative(root, p))]) {
      const p = path.join(root, f);
      if (!fs.existsSync(p)) continue;
      // whole-line comments out (in the scripts: an example in one isn't a use)
      const text = fs.readFileSync(p, 'utf8').split('\n').map((l) => (/^\s*\/\//.test(l) ? '' : l)).join('\n');
      const at = (i) => `${f}:${lineOf(text, i)}`;
      for (const m of text.matchAll(/\bt[eh]?\(\s*(['"])([^'"]+)\1(?!\s*\+)/g)) note(sec, m[2], at(m.index));
      for (const m of text.matchAll(/\bdata-i18n(?:-html)?="([^"]+)"/g)) note(sec, m[1], at(m.index));
      for (const m of text.matchAll(/\bdata-i18n-attr="([^"]+)"/g))
        for (const pair of m[1].split(';')) {
          const key = pair.split(':')[1]?.trim();
          if (key) note(sec, key, at(m.index));
        }
    }
  }
  return used;
}

const decode = (s) => s.replace(/&nbsp;/g, ' ').replace(/&lt;/g, '<').replace(/&gt;/g, '>').replace(/&quot;/g, '"').replace(/&#39;/g, "'").replace(/&amp;/g, '&');
const squash = (s) => s.replace(/\s+/g, ' ').trim();

// the English written in a page's HTML, where it has data-i18n*: the same as lang/en.json's
function checkHtml(root, sec, english, errors) {
  for (const f of SOURCES[sec].files.filter((x) => x.endsWith('.html'))) {
    const p = path.join(root, f);
    if (!fs.existsSync(p)) continue;
    const html = fs.readFileSync(p, 'utf8');
    for (const m of html.matchAll(/<([a-z][a-z0-9]*)\b([^>]*)>/gi)) {
      const [whole, tag, attrs] = m;
      const line = html.slice(0, m.index).split('\n').length;
      const attr = (name) => attrs.match(new RegExp(`\\b${name}="([^"]*)"`))?.[1];
      const inner = () => {
        // up to this element's closing tag (no element of the same kind inside it, as these are written)
        const end = html.indexOf(`</${tag}>`, m.index + whole.length);
        return end < 0 ? '' : html.slice(m.index + whole.length, end);
      };
      const key = attr('data-i18n'), hkey = attr('data-i18n-html'), akeys = attr('data-i18n-attr');
      if (key && typeof english[key] === 'string') {
        const text = inner();
        if (/</.test(text)) errors.push(`${f}:${line}: data-i18n="${key}" holds markup: use data-i18n-html`);
        else if (squash(decode(text)) !== squash(english[key])) errors.push(`${f}:${line}: "${squash(decode(text))}" isn't lang/en.json's ${sec}.${key} "${english[key]}"`);
      }
      if (hkey && typeof english[hkey] === 'string' && squash(decode(inner())) !== squash(english[hkey]))
        errors.push(`${f}:${line}: the HTML in data-i18n-html="${hkey}" isn't lang/en.json's ${sec}.${hkey}`);
      if (akeys)
        for (const pair of akeys.split(';')) {
          const [a, k] = pair.split(':').map((s) => s?.trim());
          if (!a || !k || typeof english[k] !== 'string') continue;
          const v = attr(a);
          if (v != null && decode(v) !== english[k]) errors.push(`${f}:${line}: ${a}="${decode(v)}" isn't lang/en.json's ${sec}.${k} "${english[k]}"`);
        }
    }
  }
}

export function check({ root = ROOT, format = false, unused = false } = {}) {
  const errors = [], warnings = [];
  const dir = path.join(root, 'lang');
  const enFile = path.join(dir, 'en.json');
  const en = readJson(enFile, errors);
  if (!en) return { errors, warnings };
  if (typeof en._name !== 'string') errors.push('lang/en.json: "_name" (the language\'s own name) is missing');
  for (const sec of SECTIONS) {
    if (!en[sec] || typeof en[sec] !== 'object') { errors.push(`lang/en.json: no "${sec}" section`); en[sec] = {}; }
    for (const [k, v] of Object.entries(en[sec])) checkValue(`lang/en.json ${sec}.${k}`, v, errors, true);
  }
  for (const k of Object.keys(en)) if (k !== '_name' && !SECTIONS.includes(k)) errors.push(`lang/en.json: "${k}" isn't a section (${SECTIONS.join(', ')})`);

  // the code's keys
  const used = usedKeys(root);
  for (const sec of SECTIONS)
    for (const [k, where] of used[sec]) if (!(k in en[sec])) errors.push(`${where}: "${k}" isn't in lang/en.json's ${sec} section`);
  if (unused)
    for (const sec of SECTIONS)
      for (const k of Object.keys(en[sec])) if (!used[sec].has(k)) warnings.push(`lang/en.json ${sec}.${k}: no code names it literally`);
  for (const sec of WEB) checkHtml(root, sec, en[sec], errors);

  // each language against English
  const others = fs.readdirSync(dir).filter((f) => f.endsWith('.json') && f !== 'en.json').sort();
  for (const f of others) {
    const file = path.join(dir, f);
    const code = f.slice(0, -5);
    if (!/^[A-Za-z0-9_-]{1,16}$/.test(code)) errors.push(`lang/${f}: a file is named by its language's code (es, pt-BR)`);
    const tr = readJson(file, errors);
    if (!tr) continue;
    if (typeof tr._name !== 'string' || !tr._name.trim()) errors.push(`lang/${f}: "_name" (the language's own name, as in "Español") is missing`);
    for (const k of Object.keys(tr)) if (k !== '_name' && !SECTIONS.includes(k)) errors.push(`lang/${f}: "${k}" isn't a section (${SECTIONS.join(', ')})`);
    for (const sec of SECTIONS) {
      const t = tr[sec] && typeof tr[sec] === 'object' ? tr[sec] : {};
      if (!tr[sec]) errors.push(`lang/${f}: no "${sec}" section`);
      for (const k of Object.keys(en[sec])) if (!(k in t)) errors.push(`lang/${f} ${sec}.${k}: missing (English: ${JSON.stringify(en[sec][k])})`);
      for (const [k, v] of Object.entries(t)) {
        const where = `lang/${f} ${sec}.${k}`;
        if (!(k in en[sec])) { errors.push(`${where}: English has no such key (renamed or removed?)`); continue; }
        if (!checkValue(where, v, errors, false)) continue;
        const e = en[sec][k];
        if (typeof e === 'string' && typeof v !== 'string') errors.push(`${where}: English's is one text, not plural forms`);
        if (typeof e !== 'string' && typeof v === 'string') errors.push(`${where}: English's has plural forms ({"one": …, "other": …})`);
        // a plural's forms may leave a blank out ("one": "One song"), but not bring one English hasn't
        const eb = union(e, blanks), vb = union(v, blanks);
        if (!same(eb, vb)) errors.push(`${where}: blanks ${JSON.stringify(vb)}, English has ${JSON.stringify(eb)}`);
        if (typeof v === 'string' && v.trim() === '' && String(e).trim() !== '') errors.push(`${where}: empty`);
        const et = union(e, tags), vt = union(v, tags);
        if (!same(et, vt)) errors.push(`${where}: markup ${JSON.stringify(vt)}, English has ${JSON.stringify(et)}`);
      }
    }
    if (format && !errors.some((x) => x.startsWith(`lang/${f}`))) {
      const out = { _name: tr._name };
      for (const sec of SECTIONS) {
        out[sec] = {};
        for (const k of Object.keys(en[sec])) if (k in (tr[sec] || {})) out[sec][k] = tr[sec][k];
      }
      fs.writeFileSync(file, JSON.stringify(out, null, 2) + '\n');
    }
  }
  if (format) {
    const out = { _name: en._name };
    for (const sec of SECTIONS) out[sec] = Object.fromEntries(Object.keys(en[sec]).sort().map((k) => [k, en[sec][k]]));
    fs.writeFileSync(enFile, JSON.stringify(out, null, 2) + '\n');
  }
  return { errors, warnings };
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const args = new Set(process.argv.slice(2));
  const { errors, warnings } = check({ format: args.has('--format'), unused: args.has('--unused') });
  for (const w of warnings) console.log(`warning: ${w}`);
  for (const e of errors) console.error(e);
  const langs = fs.readdirSync(path.join(ROOT, 'lang')).filter((f) => f.endsWith('.json'));
  if (errors.length) {
    console.error(`\n${errors.length} problem${errors.length === 1 ? '' : 's'} in ${langs.length} language files`);
    process.exit(1);
  }
  console.log(`lang/: ${langs.length} languages, all complete and matching English`);
}
