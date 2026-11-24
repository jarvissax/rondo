// The page's language: lang/*.json's "updater" section (docs/translating.md), every language built
// into app.js by build.mjs ("rondo:lang"), so the page fetches nothing for it.
// t(key, {name: …, count: n}) is a text in the page's language with its blanks filled ("count" also
// picks the plural form); te() is the same escaped for HTML, th() keeps the text's own <b>, <i>,
// <code> and <br> (its blanks escaped; a blank given as html(…) goes in as it is). Elements with
// data-i18n (text), data-i18n-html and data-i18n-attr="aria-label:key;title:key" get theirs from
// applyLang(), as on the admin page.
import catalog from "rondo:lang";

export const L = { code: "en", strings: catalog.strings.en || {} };
const esc = (s) => String(s ?? "").replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" })[c]);
const cached = (name, make) => (L[name]?.code === L.code ? L[name].value : (L[name] = { code: L.code, value: make() }).value);
const plurals = () => cached("_pr", () => new Intl.PluralRules(L.code));
const numbers = () => cached("_nf", () => new Intl.NumberFormat(L.code));

export function t(key, params) {
  let v = L.strings[key];
  if (v == null) return key;
  if (typeof v === "object") {
    const n = params?.count;
    const form = typeof n === "number" ? (n === 0 && v.zero != null ? "zero" : plurals().select(n)) : "other";
    v = v[form] ?? v.other;
  }
  return params ? v.replace(/\{(\w+)\}/g, (m, k) => (k in params ? (typeof params[k] === "number" ? numbers().format(params[k]) : String(params[k])) : m)) : v;
}
export const te = (key, params) => esc(t(key, params));
export const html = (s) => ({ html: String(s) });
export function th(key, params = {}) {
  // each blank is escaped on its own and goes in after the text's markup is back, so a blank (a file
  // name) never brings markup of its own
  const marks = [];
  const safe = {};
  for (const [k, v] of Object.entries(params)) {
    if (typeof v === "number") { safe[k] = v; continue; }
    marks.push(v && typeof v === "object" && "html" in v ? v.html : esc(v));
    safe[k] = `\u0001${marks.length - 1}\u0001`;
  }
  return esc(t(key, safe)).replace(/&lt;(\/?)(b|i|em|strong|code|br)&gt;/g, "<$1$2>").replace(/\u0001(\d+)\u0001/g, (m, i) => marks[+i]);
}

// the language: the one picked here before (this browser; index.html reads the same key), else the
// browser's own when the catalog has it, else English
const LANG_KEY = "rondo_updater_lang";
function pick(wanted) {
  const have = catalog.languages.map((l) => l.code);
  for (const want of wanted) {
    const w = String(want || "").trim().toLowerCase();
    if (!w) continue;
    const exact = have.find((c) => c.toLowerCase() === w);
    if (exact) return exact;
    const base = w.split(/[-_]/)[0];
    const near = have.find((c) => c.toLowerCase() === base) || have.find((c) => c.toLowerCase().split(/[-_]/)[0] === base);
    if (near) return near;
  }
  return "en";
}
let stored = null;
try { stored = localStorage.getItem(LANG_KEY); } catch { /* private mode, blocked storage */ }
use(pick([stored, ...(navigator.languages || [navigator.language])]));

function use(code) {
  L.code = code;
  L.strings = catalog.strings[code] || catalog.strings.en || {};
}

// what the page builds in its script follows a change of language too: these run after applyLang()
const followers = [];
export const onLang = (fn) => followers.push(fn);

export function applyLang(root = document) {
  for (const el of root.querySelectorAll("[data-i18n]")) el.textContent = t(el.dataset.i18n);
  for (const el of root.querySelectorAll("[data-i18n-html]")) el.innerHTML = th(el.dataset.i18nHtml);
  for (const el of root.querySelectorAll("[data-i18n-attr]"))
    for (const pair of el.dataset.i18nAttr.split(";")) {
      const [a, k] = pair.split(":").map((s) => s.trim());
      if (a && k) el.setAttribute(a, t(k));
    }
  for (const fn of followers) fn();
  document.documentElement.lang = L.code;
  // index.html hid the page until now when it expected a language other than English
  document.documentElement.classList.remove("lang-wait");
}

// the picker (#langPick): each language in its own name; this browser keeps the choice. It changes
// the page in place, so a connected Y1 stays connected.
export function langPicker(box, select) {
  if (catalog.languages.length < 2) return;
  select.replaceChildren(...catalog.languages.map((l) => {
    const o = document.createElement("option");
    o.value = l.code;
    o.lang = l.code;
    o.textContent = l.name;
    return o;
  }));
  select.value = L.code;
  box.hidden = false;
  select.addEventListener("change", () => {
    if (select.value === L.code) return;
    try { localStorage.setItem(LANG_KEY, select.value); } catch { /* private mode: for this visit only */ }
    use(select.value);
    applyLang();
  });
}
