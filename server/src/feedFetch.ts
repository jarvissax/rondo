// Fetching a podcast's RSS feed for a Rondo (GET /podcasts/feed). The address comes from the Rondo,
// so the server must not become a way into its own network: only http(s), and every host on the way
// (the feed's and each redirect's) must resolve to public addresses only. The check is made again
// when the connection opens (the lookup the socket itself uses), so a name that answers a public
// address first and a private one next (DNS rebinding) is refused too. Up to 5 redirects, 30 s in
// all, at most 60 MB (after decompression). Nothing is written to disk.

import { lookup as dnsLookup } from "node:dns/promises";
import { isIP, type LookupFunction } from "node:net";
import { pipeline, type Readable } from "node:stream";
import zlib from "node:zlib";
import { Agent, request } from "undici";

export const FEED_TIMEOUT_MS = 30_000;
export const FEED_MAX_BYTES = 60 * 1024 * 1024;
export const FEED_MAX_REDIRECTS = 5;
const USER_AGENT = "Rondo server (https://github.com/jarvissax/rondo)";
const REDIRECTS = new Set([301, 302, 303, 307, 308]);

// why a feed couldn't be read: the route answers each its own way (rss.ts)
export type FeedErrorCode = "url" | "upstream" | "timeout" | "too_big" | "not_feed";
export class FeedError extends Error {
  constructor(
    readonly code: FeedErrorCode,
    readonly status = 0, // upstream: the host's HTTP status (0: no answer)
  ) {
    super(status ? `${code} ${status}` : code);
  }
}

// ---- which addresses are public --------------------------------------------------------------

function v4(ip: string): number[] {
  return ip.split(".").map(Number);
}

function publicV4([a = 0, b = 0, c = 0]: number[]): boolean {
  if (a === 0 || a === 10 || a === 127 || a >= 224) return false; // this network, private, loopback, multicast and reserved
  if (a === 100 && b >= 64 && b <= 127) return false; // carrier-grade NAT
  if (a === 169 && b === 254) return false; // link-local (cloud metadata services)
  if (a === 172 && b >= 16 && b <= 31) return false;
  if (a === 192 && b === 168) return false;
  if (a === 192 && b === 0 && (c === 0 || c === 2)) return false; // IETF assignments, documentation
  if (a === 192 && b === 88 && c === 99) return false; // 6to4 relays
  if (a === 198 && (b === 18 || b === 19)) return false; // benchmarking
  if (a === 198 && b === 51 && c === 100) return false;
  if (a === 203 && b === 0 && c === 113) return false;
  return true;
}

// an IPv6 address's 16 bytes ("::ffff:1.2.3.4" too)
function v6(ip: string): number[] | null {
  let s = ip.split("%", 1)[0]!.toLowerCase();
  let tail: number[] = [];
  // a dotted IPv4 at the end stands for the last two groups
  const dotted = /^(.*:)(\d+\.\d+\.\d+\.\d+)$/.exec(s);
  if (dotted) {
    tail = v4(dotted[2]!);
    s = dotted[1]!.endsWith("::") ? dotted[1]! : dotted[1]!.slice(0, -1);
  }
  const halves = s.split("::");
  if (halves.length > 2) return null;
  const groups = (h: string) => (h ? h.split(":").filter((g) => g !== "") : []);
  const head = groups(halves[0]!);
  const rest = halves.length === 2 ? groups(halves[1]!) : [];
  const want = 8 - tail.length / 2;
  const fill = halves.length === 2 ? want - head.length - rest.length : 0;
  if (fill < 0 || head.length + fill + rest.length !== want) return null;
  const words = [...head, ...Array<string>(fill).fill("0"), ...rest].map((g) => parseInt(g, 16));
  if (words.some((w) => !(w >= 0 && w <= 0xffff))) return null;
  return [...words.flatMap((w) => [w >> 8, w & 0xff]), ...tail];
}

function publicV6(b: number[]): boolean {
  const zeros = (from: number, to: number) => b.slice(from, to).every((x) => x === 0);
  if (zeros(0, 10) && b[10] === 0xff && b[11] === 0xff) return publicV4(b.slice(12)); // IPv4-mapped
  if (b[0] === 0x00 && b[1] === 0x64 && b[2] === 0xff && b[3] === 0x9b && zeros(4, 12)) return publicV4(b.slice(12)); // NAT64
  if (b[0] === 0x20 && b[1] === 0x02) return publicV4(b.slice(2, 6)); // 6to4
  if ((b[0]! & 0xe0) !== 0x20) return false; // only global unicast (2000::/3): not ::1, fc00::/7, fe80::/10, ff00::/8…
  if (b[0] === 0x20 && b[1] === 0x01 && b[2]! < 0x02) return false; // 2001::/23 (Teredo, benchmarking, ORCHID)
  if (b[0] === 0x20 && b[1] === 0x01 && b[2] === 0x0d && b[3] === 0xb8) return false; // documentation
  if (b[0] === 0x3f && b[1] === 0xff && (b[2]! & 0xf0) === 0) return false; // documentation (3fff::/20)
  return true;
}

// a public address on the internet: not loopback, private, link-local, carrier-grade NAT, multicast,
// documentation or otherwise reserved (IPv4, IPv6, IPv4 inside IPv6)
export function isPublicAddress(ip: string): boolean {
  const bare = ip.replace(/^\[|\]$/g, "");
  const kind = isIP(bare.split("%", 1)[0]!);
  if (kind === 4) return publicV4(v4(bare));
  if (kind === 6) {
    const bytes = v6(bare);
    return !!bytes && publicV6(bytes);
  }
  return false;
}

// ---- fetching ------------------------------------------------------------------------------------

export type Resolve = (host: string) => Promise<string[]>;
export const systemResolve: Resolve = async (host) => (await dnsLookup(host, { all: true, verbatim: true })).map((a) => a.address);

class Blocked extends Error {
  readonly code = "ERONDO_BLOCKED";
}

// the socket's own lookup: the addresses checked as the connection opens
function guardedLookup(resolve: Resolve, allowed: (ip: string) => boolean): LookupFunction {
  return ((hostname: string, options: { all?: boolean; family?: number | string }, callback: (...args: unknown[]) => void) => {
    resolve(hostname).then(
      (found) => {
        if (!found.length) return callback(Object.assign(new Error(`no address for ${hostname}`), { code: "ENOTFOUND" }));
        if (found.some((a) => !allowed(a))) return callback(new Blocked("not a public address"));
        const family = options.family === 6 || options.family === "IPv6" ? 6 : options.family === 4 || options.family === "IPv4" ? 4 : 0;
        const list = found.map((address) => ({ address, family: isIP(address) })).filter((a) => !family || a.family === family);
        if (!list.length) return callback(Object.assign(new Error(`no IPv${family} address for ${hostname}`), { code: "ENOTFOUND" }));
        if (options.all) callback(null, list);
        else callback(null, list[0]!.address, list[0]!.family);
      },
      (err: unknown) => callback(err),
    );
  }) as LookupFunction;
}

// a body not read to its end: closed, its "aborted" error heard
function drop(stream: Readable): void {
  stream.on("error", () => {});
  stream.destroy();
}

function blocked(err: unknown): boolean {
  for (let e = err as { code?: string; cause?: unknown } | undefined, n = 0; e && n < 6; e = e.cause as typeof e, n++)
    if (e.code === "ERONDO_BLOCKED") return true;
  return false;
}

export interface Conditional {
  etag?: string;
  lastModified?: string;
}

export type FeedResponse =
  | { notModified: true; feedUrl: string }
  | { notModified: false; text: string; feedUrl: string; etag?: string; lastModified?: string };

export type FeedSource = (address: string, cond?: Conditional) => Promise<FeedResponse>;

export interface FeedFetchOptions {
  resolve?: Resolve; // tests: a made-up DNS
  allowed?: (ip: string) => boolean; // tests: a local server counts as public
  timeoutMs?: number;
  maxBytes?: number;
  maxRedirects?: number;
}

// The text of a feed's bytes: a BOM, else the XML declaration's encoding, else the HTTP charset,
// else UTF-8 (a server's default charset is wrong more often than the feed's own declaration).
export function feedText(buf: Buffer, contentType?: string): string {
  let label = "utf-8";
  if (buf[0] === 0xff && buf[1] === 0xfe) label = "utf-16le";
  else if (buf[0] === 0xfe && buf[1] === 0xff) label = "utf-16be";
  else if (!(buf[0] === 0xef && buf[1] === 0xbb && buf[2] === 0xbf)) {
    const declared = /^\s*<\?xml\b[^>]*?\bencoding\s*=\s*["']([\w.:-]+)["']/i.exec(buf.subarray(0, 300).toString("latin1"))?.[1];
    label = declared ?? /charset\s*=\s*"?([\w.:-]+)/i.exec(contentType ?? "")?.[1] ?? "utf-8";
  }
  try {
    return new TextDecoder(label).decode(buf);
  } catch {
    return new TextDecoder("utf-8").decode(buf);
  }
}

// a Rondo's feed address: http(s) only, no more than 4096 characters; null when it isn't one
export function feedAddress(v: unknown): URL | null {
  if (typeof v !== "string") return null;
  const s = v.trim();
  if (!s || s.length > 4096) return null;
  try {
    const url = new URL(s);
    if ((url.protocol !== "http:" && url.protocol !== "https:") || !url.hostname) return null;
    url.hash = "";
    return url;
  } catch {
    return null;
  }
}

export class FeedFetcher {
  private readonly agent: Agent;
  private readonly resolve: Resolve;
  private readonly allowed: (ip: string) => boolean;
  private readonly timeoutMs: number;
  private readonly maxBytes: number;
  private readonly maxRedirects: number;

  constructor(opts: FeedFetchOptions = {}) {
    this.resolve = opts.resolve ?? systemResolve;
    this.allowed = opts.allowed ?? isPublicAddress;
    this.timeoutMs = opts.timeoutMs ?? FEED_TIMEOUT_MS;
    this.maxBytes = opts.maxBytes ?? FEED_MAX_BYTES;
    this.maxRedirects = opts.maxRedirects ?? FEED_MAX_REDIRECTS;
    this.agent = new Agent({
      keepAliveTimeout: 10_000,
      connections: 16,
      connect: { lookup: guardedLookup(this.resolve, this.allowed), timeout: 15_000 },
    });
  }

  close(): Promise<void> {
    return this.agent.close();
  }

  // the host's addresses, all public (an address written as such, or what DNS answers)
  private async check(url: URL, signal: AbortSignal): Promise<void> {
    const host = url.hostname.replace(/^\[|\]$/g, "");
    if (isIP(host)) {
      if (!this.allowed(host)) throw new FeedError("url");
      return;
    }
    let found: string[];
    try {
      found = await new Promise<string[]>((ok, fail) => {
        signal.addEventListener("abort", () => fail(signal.reason), { once: true });
        this.resolve(host).then(ok, fail);
      });
    } catch (err) {
      if (signal.aborted) throw err;
      throw new FeedError("upstream"); // no such host
    }
    if (!found.length) throw new FeedError("upstream");
    if (found.some((a) => !this.allowed(a))) throw new FeedError("url");
  }

  // the body, decompressed, at most maxBytes
  private async body(res: Awaited<ReturnType<typeof request>>): Promise<Buffer> {
    const encoding = String(res.headers["content-encoding"] ?? "").trim().toLowerCase();
    const unpack =
      encoding === "gzip" || encoding === "x-gzip" ? zlib.createGunzip() : encoding === "deflate" ? zlib.createInflate() : encoding === "br" ? zlib.createBrotliDecompress() : null;
    const stream: Readable = unpack ? pipeline(res.body, unpack, () => {}) : res.body;
    const chunks: Buffer[] = [];
    let size = 0;
    for await (const chunk of stream) {
      size += (chunk as Buffer).length;
      if (size > this.maxBytes) {
        drop(res.body);
        if (unpack) drop(unpack);
        throw new FeedError("too_big");
      }
      chunks.push(chunk as Buffer);
    }
    return Buffer.concat(chunks, size);
  }

  // `feedUrl`: the address after permanent redirects (301, 308) only, as long as no temporary one
  // came before them: where the feed lives from now on
  fetch: FeedSource = async (address, cond = {}) => {
    const first = feedAddress(address);
    if (!first) throw new FeedError("url");
    const ctl = new AbortController();
    const timer = setTimeout(() => ctl.abort(new FeedError("timeout")), this.timeoutMs);
    // an address with a user name and password (some private feeds): sent as Basic auth to that host only
    const credentials = (u: URL) => (u.username ? `Basic ${Buffer.from(`${decodeURIComponent(u.username)}:${decodeURIComponent(u.password)}`).toString("base64")}` : undefined);
    const bare = (u: URL) => Object.assign(new URL(u), { username: "", password: "" });
    let url = first;
    let auth = credentials(first);
    let feedUrl = first.toString();
    let permanent = true;
    try {
      for (let hop = 0; ; hop++) {
        await this.check(url, ctl.signal);
        const headers: Record<string, string> = {
          "user-agent": USER_AGENT,
          accept: "application/rss+xml, application/xml;q=0.9, text/xml;q=0.9, */*;q=0.8",
          "accept-encoding": "gzip, deflate, br",
        };
        if (auth) headers.authorization = auth;
        if (cond.etag) headers["if-none-match"] = cond.etag;
        if (cond.lastModified) headers["if-modified-since"] = cond.lastModified;
        const res = await request(bare(url), {
          method: "GET",
          dispatcher: this.agent,
          headers,
          signal: ctl.signal,
          headersTimeout: this.timeoutMs,
          bodyTimeout: this.timeoutMs,
        });
        const status = res.statusCode;
        const location = res.headers.location;
        if (REDIRECTS.has(status) && typeof location === "string" && location) {
          await res.body.dump().catch(() => {});
          if (hop >= this.maxRedirects) throw new FeedError("upstream", status);
          let next: URL;
          try {
            next = new URL(location, url);
          } catch {
            throw new FeedError("upstream", status);
          }
          if (next.protocol !== "http:" && next.protocol !== "https:") throw new FeedError("url");
          next.hash = "";
          if (next.origin !== url.origin) auth = credentials(next);
          if (permanent && (status === 301 || status === 308)) feedUrl = next.toString();
          else permanent = false;
          url = next;
          continue;
        }
        if (status === 304 && (cond.etag || cond.lastModified)) {
          await res.body.dump().catch(() => {});
          return { notModified: true, feedUrl };
        }
        if (status < 200 || status > 299) {
          await res.body.dump().catch(() => {});
          throw new FeedError("upstream", status);
        }
        if (Number(res.headers["content-length"]) > this.maxBytes) {
          drop(res.body);
          throw new FeedError("too_big");
        }
        const buf = await this.body(res);
        const header = (k: string) => (typeof res.headers[k] === "string" ? (res.headers[k] as string) : undefined);
        return { notModified: false, text: feedText(buf, header("content-type")), feedUrl, etag: header("etag"), lastModified: header("last-modified") };
      }
    } catch (err) {
      if (err instanceof FeedError) throw err;
      if (ctl.signal.aborted && ctl.signal.reason instanceof FeedError) throw ctl.signal.reason;
      if (blocked(err)) throw new FeedError("url");
      throw new FeedError("upstream"); // no answer: refused, reset, TLS, a broken gzip
    } finally {
      clearTimeout(timer);
    }
  };
}
