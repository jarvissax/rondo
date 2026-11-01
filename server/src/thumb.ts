// Thumbnails for the Y1: fetched from Google's image hosts, sized for the screen, re-encoded as JPEG.

// Google's image hosts; www.gstatic.com serves the artwork of auto playlists such as Liked Music
const THUMB_HOSTS = [".ytimg.com", ".googleusercontent.com", ".ggpht.com", ".gstatic.com"];

export function isThumbHost(url: URL): boolean {
  return url.protocol === "https:" && THUMB_HOSTS.some((h) => url.hostname.endsWith(h));
}

export const MIN_THUMB_PX = 16;
export const MAX_THUMB_PX = 720;
export const DEFAULT_THUMB_PX = 64;

// a w/h query value clamped to 16..720; undefined when absent or not a number
export function thumbPx(v: unknown): number | undefined {
  const n = typeof v === "string" && v.trim() ? Math.round(Number(v)) : NaN;
  return Number.isFinite(n) ? Math.min(Math.max(n, MIN_THUMB_PX), MAX_THUMB_PX) : undefined;
}

// Google's image server (googleusercontent/ggpht) sizes an image by the options after the last "=":
// "=w120-h120-l90-rj", "=w544-h544-p-l90-rj", "=s88-c-k-c0x00ffffff-no-rj". Rewrite the size so the
// short side is at least `px`, keeping the other options (quality, crop, format). URLs without such
// a token, and i.ytimg.com video thumbnails (signed for their size), are returned unchanged.
export function sizedThumbUrl(src: URL, px: number): URL {
  if (!/(^|\.)(googleusercontent|ggpht)\.com$/.test(src.hostname)) return src;
  const path = src.pathname;
  const eq = path.lastIndexOf("=");
  if (eq < 0 || path.includes("/", eq)) return src;
  const opts = path.slice(eq + 1).split("-");
  const size = (k: string): number | undefined => {
    const m = opts.map((o) => new RegExp(`^${k}(\\d+)$`).exec(o)).find(Boolean);
    return m ? Number(m[1]) : undefined;
  };
  const w = size("w");
  const h = size("h");
  const s = size("s");
  if (w === undefined && h === undefined && s === undefined) return src;
  // keep the aspect of a w×h pair: scale it so its short side becomes px
  const scale = w && h ? px / Math.min(w, h) : 1;
  const sized = opts.map((o) => {
    if (w !== undefined && /^w\d+$/.test(o)) return `w${h ? Math.round(w * scale) : px}`;
    if (h !== undefined && /^h\d+$/.test(o)) return `h${w ? Math.round(h * scale) : px}`;
    if (s !== undefined && /^s\d+$/.test(o)) return `s${px}`;
    return o;
  });
  const out = new URL(src);
  out.pathname = `${path.slice(0, eq + 1)}${sized.join("-")}`;
  return out;
}
