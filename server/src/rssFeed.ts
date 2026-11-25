// A podcast's RSS feed read into the "compact feed" (phase 3, RSS podcasts). The Y1 has its own
// parser (native/direct/rss_parse.cpp) that follows the same rules, so a feed reads the same in both
// modes: RSS 2.0 only (<rss><channel>), the itunes:, podcast: and content: elements by their usual
// prefixes, HTML in titles and notes turned into plain text. Tolerant of what feeds really do: CDATA,
// entities from HTML, raw HTML inside a description, a tag left open.

export interface FeedEpisode {
  guid: string;
  title: string;
  date: number; // unix seconds; 0 when the feed's date can't be read
  duration: number; // seconds; 0 unknown
  url: string; // the enclosure: what plays
  type: string; // its MIME type ("audio/mpeg"), lower case; "" unknown
  size: number; // bytes, as the feed says (0 unknown)
  image: string;
  chapters: string; // podcast:chapters' JSON address
  season: number;
  episode: number;
  episodeType: "full" | "trailer" | "bonus";
  notes: string; // plain text, at most 4000 bytes
}

export interface FeedChannel {
  title: string;
  author: string;
  description: string; // plain text, at most 2000 bytes
  art: string;
  link: string;
  language: string; // lower case ("en-us")
  serial: boolean;
  explicit: boolean;
  complete: boolean;
  newFeedUrl: string;
}

export interface ParsedFeed extends FeedChannel {
  count: number; // every episode the feed has (with an enclosure)
  episodes: FeedEpisode[]; // in the feed's own order
}

export const DESCRIPTION_BYTES = 2000;
export const NOTES_BYTES = 4000;

// ---- entities ------------------------------------------------------------------------------

const XML_ENTITIES: Record<string, string> = { amp: "&", lt: "<", gt: ">", quot: '"', apos: "'" };

// Latin-1's names in code point order, 160 (nbsp) to 255 (yuml)
const LATIN1 =
  "nbsp iexcl cent pound curren yen brvbar sect uml copy ordf laquo not shy reg macr deg plusmn sup2 sup3 acute micro para middot cedil sup1 ordm raquo frac14 frac12 frac34 iquest Agrave Aacute Acirc Atilde Auml Aring AElig Ccedil Egrave Eacute Ecirc Euml Igrave Iacute Icirc Iuml ETH Ntilde Ograve Oacute Ocirc Otilde Ouml times Oslash Ugrave Uacute Ucirc Uuml Yacute THORN szlig agrave aacute acirc atilde auml aring aelig ccedil egrave eacute ecirc euml igrave iacute icirc iuml eth ntilde ograve oacute ocirc otilde ouml divide oslash ugrave uacute ucirc uuml yacute thorn yuml".split(
    " ",
  );
export const HTML_ENTITIES: Record<string, string> = {
  ...XML_ENTITIES,
  ...Object.fromEntries(LATIN1.map((name, i) => [name, String.fromCodePoint(160 + i)])),
  OElig: "Œ", oelig: "œ", Scaron: "Š", scaron: "š", Yuml: "Ÿ", fnof: "ƒ",
  circ: "ˆ", tilde: "˜", ensp: " ", emsp: " ", thinsp: " ", zwnj: "‌",
  zwj: "‍", lrm: "‎", rlm: "‏", ndash: "–", mdash: "—", lsquo: "‘",
  rsquo: "’", sbquo: "‚", ldquo: "“", rdquo: "”", bdquo: "„", dagger: "†",
  Dagger: "‡", bull: "•", hellip: "…", permil: "‰", prime: "′", Prime: "″",
  lsaquo: "‹", rsaquo: "›", oline: "‾", frasl: "⁄", euro: "€", trade: "™",
  larr: "←", uarr: "↑", rarr: "→", darr: "↓", harr: "↔", minus: "−",
  spades: "♠", clubs: "♣", hearts: "♥", diams: "♦",
};

const ENTITY = /&(#[0-9]+|#[xX][0-9a-fA-F]+|[A-Za-z][A-Za-z0-9]*);/g;

function decodeEntities(s: string, names: Record<string, string>): string {
  if (!s.includes("&")) return s;
  return s.replace(ENTITY, (whole, ref: string) => {
    if (ref[0] !== "#") return Object.hasOwn(names, ref) ? names[ref]! : whole;
    const code = ref[1] === "x" || ref[1] === "X" ? parseInt(ref.slice(2), 16) : parseInt(ref.slice(1), 10);
    if (!(code > 0 && code <= 0x10ffff) || (code >= 0xd800 && code <= 0xdfff)) return "�";
    return String.fromCodePoint(code);
  });
}

// An element's content as XML says it: CDATA sections are text as they are, comments go, the XML
// entities (the five and numeric ones) are decoded. Tags inside (raw HTML in a description) stay
// as they are, for plainText() to read as HTML.
export function xmlText(raw: string): string {
  if (!raw.includes("<") && !raw.includes("&")) return raw;
  let out = "";
  let i = 0;
  while (i < raw.length) {
    const lt = raw.indexOf("<", i);
    if (lt < 0) {
      out += decodeEntities(raw.slice(i), XML_ENTITIES);
      break;
    }
    out += decodeEntities(raw.slice(i, lt), XML_ENTITIES);
    if (raw.startsWith("<![CDATA[", lt)) {
      const end = raw.indexOf("]]>", lt + 9);
      out += raw.slice(lt + 9, end < 0 ? raw.length : end);
      i = end < 0 ? raw.length : end + 3;
    } else if (raw.startsWith("<!--", lt)) {
      const end = raw.indexOf("-->", lt + 4);
      i = end < 0 ? raw.length : end + 3;
    } else {
      // a tag: kept, its attribute values decoded with the rest
      const end = raw.indexOf(">", lt + 1);
      const stop = end < 0 ? raw.length : end + 1;
      out += decodeEntities(raw.slice(lt, stop), XML_ENTITIES);
      i = stop;
    }
  }
  return out;
}

// ---- plain text from HTML ------------------------------------------------------------------

// the end of these gives a line break (as <br> does)
const BLOCKS = new Set(["p", "div", "li", "h1", "h2", "h3", "h4", "h5", "h6", "tr", "blockquote", "pre", "ul", "ol"]);
const TAG = /<(\/?)([A-Za-z][A-Za-z0-9:-]*)\b[^>]*>/g;

// at most `max` bytes of UTF-8, cut on a character's boundary, with "…" (3 bytes) inside the limit
export function cutBytes(s: string, max: number): string {
  if (s.length <= max / 3 || Buffer.byteLength(s) <= max) return s;
  const buf = Buffer.from(s);
  let end = Math.max(0, max - 3);
  while (end > 0 && (buf[end]! & 0xc0) === 0x80) end--; // not inside a character
  return `${buf.subarray(0, end).toString("utf8").trimEnd()}…`;
}

// Plain text from HTML that was decoded from XML first (xmlText): line breaks for <br> and the end
// of block elements, "• " before a list item, script and style dropped with what's in them, other
// tags dropped, HTML's entities decoded; spaces and tabs collapse, lines trimmed, at most one empty
// line in a row. `oneLine` (titles): line breaks are spaces too. `maxBytes`: cut with "…".
export function plainText(html: string, opts: { oneLine?: boolean; maxBytes?: number } = {}): string {
  let s = html.replace(/\r\n?/g, "\n");
  if (s.includes("<")) {
    s = s
      .replace(/<!--[\s\S]*?(-->|$)/g, "")
      .replace(/<(script|style)\b[^>]*>[\s\S]*?(<\/\1\s*>|$)/gi, "")
      .replace(TAG, (_m, close: string, name: string) => {
        const tag = name.toLowerCase();
        if (!close && tag === "br") return "\n";
        if (!close && tag === "li") return "• ";
        return close && BLOCKS.has(tag) ? "\n" : "";
      });
  }
  s = decodeEntities(s, HTML_ENTITIES)
    .replace(/[ \t\f\v ]+/g, " ")
    .replace(/ ?\n ?/g, "\n")
    .replace(/\n{3,}/g, "\n\n")
    .trim();
  if (opts.oneLine) s = s.replace(/\n+/g, " ").replace(/ {2,}/g, " ");
  return opts.maxBytes ? cutBytes(s, opts.maxBytes) : s;
}

// ---- dates and durations -------------------------------------------------------------------

const MONTHS = ["jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"];
const ZONES: Record<string, number> = { gmt: 0, ut: 0, utc: 0, z: 0, est: -5, edt: -4, cst: -6, cdt: -5, mst: -7, mdt: -6, pst: -8, pdt: -7 };
// "Tue, 01 May 2024 10:00:00 +0000": weekday optional, day 1-2 digits, month by its first three
// letters, year 2 or 4 digits, seconds optional, then the zone (or nothing)
const RFC822 = /^(?:[A-Za-z]+\.?,?\s*)?(\d{1,2})\s+([A-Za-z]{3,})\.?\s+(\d{2}|\d{4})\s+(\d{1,2}):(\d{2})(?::(\d{2}))?(?:\s+(\S+).*)?$/;
const ISO8601 = /^(\d{4})-(\d{2})-(\d{2})(?:[Tt ](\d{2}):(\d{2})(?::(\d{2})(?:[.,]\d+)?)?\s*([Zz]|[+-]\d{2}(?::?\d{2})?)?)?$/;

// minutes east of UTC for a zone ("+0530", "-05:00", "EST"); unknown or missing: UTC
function zoneMinutes(zone: string | undefined): number {
  if (!zone) return 0;
  const m = /^([+-])(\d{2}):?(\d{2})?$/.exec(zone);
  if (m) return (m[1] === "-" ? -1 : 1) * (Number(m[2]) * 60 + Number(m[3] ?? 0));
  return (ZONES[zone.toLowerCase()] ?? 0) * 60;
}

function utcSeconds(y: number, mo: number, d: number, h: number, mi: number, s: number, zone: number): number {
  if (mo < 0 || mo > 11 || d < 1 || d > 31 || h > 23 || mi > 59 || s > 60) return 0;
  const ms = Date.UTC(y, mo, d, h, mi, Math.min(s, 59)) - zone * 60_000;
  return ms > 0 ? Math.floor(ms / 1000) : 0;
}

// a pubDate (RFC 822 / 1123 and the variants feeds use, or ISO 8601) in unix seconds; 0 unreadable
export function parseDate(text: string): number {
  const s = text.trim();
  let m = RFC822.exec(s);
  if (m) {
    const month = MONTHS.indexOf(m[2]!.slice(0, 3).toLowerCase());
    if (month < 0) return 0;
    let year = Number(m[3]);
    if (m[3]!.length === 2) year += year < 70 ? 2000 : 1900;
    return utcSeconds(year, month, Number(m[1]), Number(m[4]), Number(m[5]), Number(m[6] ?? 0), zoneMinutes(m[7]));
  }
  m = ISO8601.exec(s);
  if (m) return utcSeconds(Number(m[1]), Number(m[2]) - 1, Number(m[3]), Number(m[4] ?? 0), Number(m[5] ?? 0), Number(m[6] ?? 0), zoneMinutes(m[7]));
  return 0;
}

// itunes:duration in seconds: "H:MM:SS", "MM:SS" (minutes past 59 too) or seconds ("3600.5" → 3600)
export function parseDuration(text: string): number {
  const s = text.trim();
  if (/^\d+(\.\d+)?$/.test(s)) return Math.floor(Number(s));
  const m = /^(?:(\d+):)?(\d+):(\d+)(\.\d+)?$/.exec(s);
  if (!m) return 0;
  return Number(m[1] ?? 0) * 3600 + Number(m[2]) * 60 + Number(m[3]);
}

// a whole number ≥ 0 ("12345678", "3.0"); anything else 0
export function wholeNumber(text: string): number {
  const s = text.trim();
  return /^\d+(\.\d+)?$/.test(s) ? Math.min(Math.floor(Number(s)), Number.MAX_SAFE_INTEGER) : 0;
}

const TYPES: Record<string, string> = {
  mp3: "audio/mpeg", m4a: "audio/mp4", m4b: "audio/mp4", aac: "audio/aac", ogg: "audio/ogg", opus: "audio/ogg",
  mp4: "video/mp4", m4v: "video/mp4", mov: "video/quicktime",
};

// an enclosure's type from its address's path ("…/ep.mp3?x=1" → audio/mpeg); "" when unknown
export function typeFromUrl(url: string): string {
  let path: string;
  try {
    path = new URL(url).pathname;
  } catch {
    path = url.split(/[?#]/, 1)[0]!;
  }
  const name = path.slice(path.lastIndexOf("/") + 1);
  const dot = name.lastIndexOf(".");
  return dot < 0 ? "" : (TYPES[name.slice(dot + 1).toLowerCase()] ?? "");
}

// ---- the XML ---------------------------------------------------------------------------------

// the channel's and an item's elements whose text is read (the others: their attributes, or what's in them)
const CHANNEL_TEXT = new Set([
  "title", "link", "language", "description", "itunes:summary", "itunes:author", "managingEditor",
  "itunes:type", "itunes:explicit", "itunes:new-feed-url", "itunes:complete",
]);
const ITEM_TEXT = new Set([
  "guid", "title", "itunes:title", "pubDate", "itunes:duration", "itunes:season", "itunes:episode",
  "itunes:episodeType", "content:encoded", "itunes:summary", "description",
]);

type Attrs = Record<string, string>;
interface Enclosure {
  url: string;
  type: string;
  length: string;
}
interface ItemRaw {
  text: Record<string, string>;
  enclosures: Enclosure[];
  image: string;
  chapters: string;
}

const isSpace = (c: number) => c === 32 || c === 9 || c === 10 || c === 13;
const isNameStart = (c: number) => (c >= 65 && c <= 90) || (c >= 97 && c <= 122) || c === 95 || c === 58 || c > 127;

// where the content of an element that started at `from` ends: the index of its "</name>" and the
// index after it (CDATA and comments skipped over); null when it never closes
function closingTag(xml: string, from: number, name: string): [number, number] | null {
  let i = from;
  for (;;) {
    const lt = xml.indexOf("<", i);
    if (lt < 0) return null;
    if (xml.startsWith("<![CDATA[", lt)) {
      const end = xml.indexOf("]]>", lt + 9);
      if (end < 0) return null;
      i = end + 3;
    } else if (xml.startsWith("<!--", lt)) {
      const end = xml.indexOf("-->", lt + 4);
      if (end < 0) return null;
      i = end + 3;
    } else if (xml.startsWith(name, lt + 2) && xml.charCodeAt(lt + 1) === 47) {
      let j = lt + 2 + name.length;
      while (j < xml.length && isSpace(xml.charCodeAt(j))) j++;
      if (xml[j] === ">") return [lt, j + 1];
      i = lt + 1;
    } else i = lt + 1;
  }
}

// a start tag at `lt` ("<name a="1" b='2'>" or "…/>"): its name, attributes, where it ends
function startTag(xml: string, lt: number): { name: string; attrs: Attrs; end: number; empty: boolean } {
  const n = xml.length;
  let j = lt + 1;
  while (j < n && !isSpace(xml.charCodeAt(j)) && xml[j] !== ">" && xml[j] !== "/") j++;
  const name = xml.slice(lt + 1, j);
  const attrs: Attrs = {};
  for (;;) {
    while (j < n && isSpace(xml.charCodeAt(j))) j++;
    if (j >= n) return { name, attrs, end: n, empty: false };
    if (xml[j] === ">") return { name, attrs, end: j + 1, empty: false };
    if (xml[j] === "/") {
      if (xml[j + 1] === ">") return { name, attrs, end: j + 2, empty: true };
      j++;
      continue;
    }
    const a = j;
    while (j < n && !isSpace(xml.charCodeAt(j)) && xml[j] !== "=" && xml[j] !== ">" && xml[j] !== "/") j++;
    const attr = xml.slice(a, j);
    while (j < n && isSpace(xml.charCodeAt(j))) j++;
    let value = "";
    if (xml[j] === "=") {
      j++;
      while (j < n && isSpace(xml.charCodeAt(j))) j++;
      const q = xml[j];
      if (q === '"' || q === "'") {
        const close = xml.indexOf(q, j + 1);
        value = xml.slice(j + 1, close < 0 ? n : close);
        j = close < 0 ? n : close + 1;
      } else {
        const v = j;
        while (j < n && !isSpace(xml.charCodeAt(j)) && xml[j] !== ">") j++;
        value = xml.slice(v, j);
      }
    }
    if (attr && !(attr in attrs)) attrs[attr] = decodeEntities(value, XML_ENTITIES);
  }
}

// the first non-empty value of each key
function keep(into: Record<string, string>, key: string, value: string): void {
  if (!into[key]?.trim()) into[key] = value;
}

const firstText = (...texts: string[]): string => texts.find((t) => t !== "") ?? "";
const yes = (v: string | undefined, ...words: string[]) => words.includes((v ?? "").trim().toLowerCase());

function episodeOf(item: ItemRaw): FeedEpisode | undefined {
  const withUrl = item.enclosures.filter((e) => e.url.trim());
  const enc = withUrl.find((e) => /^(audio|video)\//i.test(e.type.trim())) ?? withUrl[0];
  if (!enc) return undefined;
  const t = (k: string) => xmlText(item.text[k] ?? "");
  const url = enc.url.trim();
  const kind = t("itunes:episodeType").trim().toLowerCase();
  return {
    guid: t("guid").trim() || url,
    title: firstText(plainText(t("title"), { oneLine: true }), plainText(t("itunes:title"), { oneLine: true })),
    date: parseDate(t("pubDate")),
    duration: parseDuration(t("itunes:duration")),
    url,
    type: enc.type.trim().toLowerCase() || typeFromUrl(url),
    size: wholeNumber(enc.length),
    image: item.image,
    chapters: item.chapters,
    season: wholeNumber(t("itunes:season")),
    episode: wholeNumber(t("itunes:episode")),
    episodeType: kind === "trailer" || kind === "bonus" ? kind : "full",
    notes: firstText(
      ...["content:encoded", "itunes:summary", "description"].map((k) => plainText(t(k), { maxBytes: NOTES_BYTES })),
    ),
  };
}

// what's between "<!" or "<?" and its end (a comment, CDATA outside what's read, the XML declaration,
// a DOCTYPE with its [subset]): the index after it, or -1 when it never ends
function skipMarkup(xml: string, lt: number): number {
  if (xml.startsWith("<!--", lt)) {
    const end = xml.indexOf("-->", lt + 4);
    return end < 0 ? -1 : end + 3;
  }
  if (xml.startsWith("<![CDATA[", lt)) {
    const end = xml.indexOf("]]>", lt + 9);
    return end < 0 ? -1 : end + 3;
  }
  if (xml[lt + 1] === "?") {
    const end = xml.indexOf("?>", lt + 2);
    return end < 0 ? -1 : end + 2;
  }
  let brackets = 0;
  for (let j = lt + 2; j < xml.length; j++) {
    const c = xml[j];
    if (c === "[") brackets++;
    else if (c === "]") brackets--;
    else if (c === ">" && brackets <= 0) return j + 1;
  }
  return -1;
}

// The feed, or null when it isn't one (not XML, not <rss>, no <channel>).
export function parseFeed(xml: string): ParsedFeed | null {
  const n = xml.length;
  const stack: string[] = []; // the elements open now: rss, channel, item, …
  const ch: Record<string, string> = {}; // the channel's texts (raw) and attributes
  const episodes: FeedEpisode[] = [];
  let root = false;
  let channel = false; // inside the first <channel>
  let item: ItemRaw | null = null; // inside one of its <item>s
  let i = 0;
  while (i < n) {
    const lt = xml.indexOf("<", i);
    if (lt < 0) break;
    const next = xml.charCodeAt(lt + 1);
    if (next === 33 /* ! */ || next === 63 /* ? */) {
      i = skipMarkup(xml, lt);
      if (i < 0) break;
      continue;
    }
    if (next === 47 /* / */) {
      const end = xml.indexOf(">", lt + 2);
      if (end < 0) break;
      const at = stack.lastIndexOf(xml.slice(lt + 2, end).trim());
      i = end + 1;
      if (at < 0) continue; // a close tag nothing opened
      // it closes what was left open inside it too (a <br> in raw HTML, a forgotten </p>)
      if (item && at <= 2) {
        const ep = episodeOf(item);
        if (ep) episodes.push(ep);
        item = null;
      }
      stack.length = at;
      if (at === 0 || (at === 1 && channel)) break;
      continue;
    }
    if (!isNameStart(next)) {
      i = lt + 1; // a lone "<" in text
      continue;
    }
    const tag = startTag(xml, lt);
    i = tag.end;
    const depth = stack.length;
    let text: string | undefined; // an element whose content is read whole: its key
    if (depth === 0) {
      if (tag.name !== "rss") return null;
      root = true;
    } else if (depth === 1) {
      if (tag.name === "channel") channel = true;
    } else if (channel && depth === 2) {
      if (tag.name === "item") item = tag.empty ? null : { text: {}, enclosures: [], image: "", chapters: "" };
      else if (tag.name === "itunes:image") keep(ch, "#itunes:image", (tag.attrs.href ?? "").trim());
      else if (CHANNEL_TEXT.has(tag.name)) text = `c:${tag.name}`;
    } else if (channel && depth === 3) {
      if (item) {
        if (tag.name === "enclosure") item.enclosures.push({ url: tag.attrs.url ?? "", type: tag.attrs.type ?? "", length: tag.attrs.length ?? "" });
        else if (tag.name === "itunes:image") item.image ||= (tag.attrs.href ?? "").trim();
        else if (tag.name === "podcast:chapters") item.chapters ||= (tag.attrs.url ?? "").trim();
        else if (ITEM_TEXT.has(tag.name)) text = `i:${tag.name}`;
      } else if (stack[2] === "image" && tag.name === "url") text = "c:image/url";
      else if (stack[2] === "itunes:owner" && tag.name === "itunes:name") text = "c:owner";
    }
    if (tag.empty) continue;
    if (text) {
      const end = closingTag(xml, i, tag.name);
      if (end) {
        const raw = xml.slice(i, end[0]);
        if (text.startsWith("c:")) keep(ch, text.slice(2), raw);
        else keep(item!.text, text.slice(2), raw);
        i = end[1];
        continue;
      }
    }
    stack.push(tag.name);
  }
  if (!root || !channel) return null;

  const c = (k: string) => xmlText(ch[k] ?? "");
  const line = (k: string) => plainText(c(k), { oneLine: true });
  const description = (k: string) => plainText(c(k), { maxBytes: DESCRIPTION_BYTES });
  return {
    title: line("title"),
    author: firstText(line("itunes:author"), line("owner"), line("managingEditor")),
    description: firstText(description("itunes:summary"), description("description")),
    art: ch["#itunes:image"] || c("image/url").trim(),
    link: c("link").trim(),
    language: c("language").trim().toLowerCase(),
    serial: yes(c("itunes:type"), "serial"),
    explicit: yes(c("itunes:explicit"), "yes", "true", "explicit"),
    complete: yes(c("itunes:complete"), "yes"),
    newFeedUrl: c("itunes:new-feed-url").trim(),
    count: episodes.length,
    episodes,
  };
}
