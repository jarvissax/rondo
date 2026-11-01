// Sign-in with a code, the way a smart TV signs in (docs/dev/server-api.md): the admin page shows a code,
// the owner enters it at google.com/device, and the server keeps the OAuth tokens on the data volume
// (TV_TOKENS_FILE, 0600), refreshing them as youtubei.js asks. YouTube accepts these tokens only
// from its TV client (tested 2026-10-31: WEB and YTMUSIC answer 400), so they serve the YouTube
// feeds (home, subscriptions, history) and the account's playlists and liked music. YouTube Music's
// own home and history still need cookies.txt. Without either, both services run signed out.

import { existsSync } from "node:fs";
import { mkdir, readFile, rename, rm, writeFile } from "node:fs/promises";
import { dirname } from "node:path";
import { Innertube } from "youtubei.js";
import type { Chunk } from "./paging.js";

export interface TvItem {
  kind: "video" | "playlist";
  id: string;
  title: string;
  subtitle: string;
  durationSec?: number;
  thumbUrl?: string; // the raw ytimg URL; browse.ts turns it into a /thumb link
}

export interface TvPending {
  url: string;
  code: string;
  expiresAt: number;
}

export interface TvAccountInfo {
  name: string;
  handle: string | null;
}

interface Log {
  info: (obj: object | string, msg?: string) => void;
  warn: (obj: object | string, msg?: string) => void;
}

const VIDEO_ID = /^[A-Za-z0-9_-]{11}$/;

// --- reading TV pages: tileRenderer (most feeds) and lockupViewModel (search, newer shelves) ---

type Raw = Record<string, any>;

const rawText = (t: any): string =>
  typeof t === "string" ? t : typeof t?.simpleText === "string" ? t.simpleText : typeof t?.content === "string" ? t.content : Array.isArray(t?.runs) ? t.runs.map((r: Raw) => r?.text ?? "").join("") : "";

// "2:44", "1:02:03"; anything else (LIVE, SHORTS, UPCOMING) is no duration
export function clockSeconds(s: string): number | undefined {
  if (!/^\d{1,3}(:\d{2}){1,2}$/.test(s.trim())) return undefined;
  return s.trim().split(":").reduce((acc, part) => acc * 60 + Number(part), 0) || undefined;
}

function biggest(thumbs: any): string | undefined {
  const list: Raw[] = Array.isArray(thumbs) ? thumbs : [];
  let best: Raw | undefined;
  for (const t of list) if (typeof t?.url === "string" && (!best || (t.width ?? 0) > (best.width ?? 0))) best = t;
  return best?.url;
}

// the channel line and the views · age line, bullets dropped
function tileLines(meta: Raw | undefined): string[] {
  return (meta?.lines ?? [])
    .map((l: Raw) =>
      (l?.lineRenderer?.items ?? [])
        .map((i: Raw) => rawText(i?.lineItemRenderer?.text).trim())
        .filter((t: string) => t && t !== "•")
        .join(" · "),
    )
    .filter(Boolean);
}

function fromTile(t: Raw): TvItem | undefined {
  const type = String(t.contentType ?? "");
  const header = t.header?.tileHeaderRenderer;
  const meta = t.metadata?.tileMetadataRenderer;
  // carousel tiles (topic pages) have no metadata block; their long-press menu names them
  const menu = t.onLongPressCommand?.showMenuCommand;
  const title = (rawText(meta?.title) || rawText(menu?.title)).trim();
  const lines = meta ? tileLines(meta) : [rawText(menu?.subtitle).split("•")[0]!.trim()].filter(Boolean);
  const thumbUrl = biggest(header?.thumbnail?.thumbnails);
  if (type.endsWith("_PLAYLIST")) {
    const browseId: string | undefined = t.onSelectCommand?.browseEndpoint?.browseId;
    const raw = browseId ?? t.contentId;
    if (typeof raw !== "string" || !raw) return undefined;
    const id = raw.startsWith("VL") && raw.length > 3 ? raw.slice(2) : raw;
    return { kind: "playlist", id, title, subtitle: lines.join(" · "), thumbUrl };
  }
  if (!type.endsWith("_VIDEO") || t.onSelectCommand?.reelWatchEndpoint) return undefined; // shorts, channels
  const id = t.onSelectCommand?.watchEndpoint?.videoId ?? t.contentId;
  if (!VIDEO_ID.test(String(id))) return undefined;
  const overlay = (header?.thumbnailOverlays ?? []).map((o: Raw) => rawText(o?.thumbnailOverlayTimeStatusRenderer?.text)).find(Boolean) ?? "";
  const durationSec = clockSeconds(overlay);
  if (!durationSec) return undefined; // live streams, premieres and promos carry no duration
  return { kind: "video", id, title, subtitle: lines.slice(0, 2).join(" · "), durationSec, thumbUrl };
}

function fromLockup(l: Raw): TvItem | undefined {
  const type = String(l.contentType ?? "");
  const json = JSON.stringify(l);
  if (json.includes("reelWatchEndpoint")) return undefined; // shorts
  const meta = l.metadata?.lockupMetadataViewModel;
  const title = rawText(meta?.title).trim();
  const parts: string[] = (meta?.metadata?.contentMetadataViewModel?.metadataRows ?? [])
    .flatMap((r: Raw) => r?.metadataParts ?? [])
    .map((p: Raw) => rawText(p?.text).trim())
    .filter(Boolean);
  const thumbUrl = biggest(l.contentImage?.thumbnailViewModel?.image?.sources ?? l.contentImage?.collectionThumbnailViewModel?.primaryThumbnail?.thumbnailViewModel?.image?.sources);
  if (type.endsWith("_PLAYLIST")) {
    if (typeof l.contentId !== "string" || !l.contentId) return undefined;
    return { kind: "playlist", id: l.contentId, title, subtitle: parts.join(" · "), thumbUrl };
  }
  // TV search calls music videos LOCKUP_CONTENT_TYPE_MUSIC
  if (!/_(VIDEO|MUSIC)$/.test(type) || !VIDEO_ID.test(String(l.contentId))) return undefined;
  const durationSec = badgeTexts(l).map(clockSeconds).find(Boolean);
  if (!durationSec) return undefined;
  return { kind: "video", id: l.contentId, title, subtitle: parts.slice(0, 2).join(" · "), durationSec, thumbUrl };
}

// the texts of a lockup's thumbnail badges ("2:44", "LIVE", "Mix")
function badgeTexts(o: unknown, out: string[] = []): string[] {
  if (!o || typeof o !== "object") return out;
  const b = (o as Raw).thumbnailBadgeViewModel;
  if (typeof b?.text === "string") out.push(b.text);
  for (const v of Object.values(o as Raw)) badgeTexts(v, out);
  return out;
}

// every video and playlist a TV response shows, in page order, once each
export function tvItems(data: unknown): TvItem[] {
  const out: TvItem[] = [];
  const seen = new Set<string>();
  const walk = (o: unknown): void => {
    if (!o || typeof o !== "object") return;
    const r = o as Raw;
    const item = r.tileRenderer ? fromTile(r.tileRenderer) : r.lockupViewModel ? fromLockup(r.lockupViewModel) : undefined;
    if (item && !seen.has(item.id)) {
      seen.add(item.id);
      out.push(item);
    }
    if (r.tileRenderer || r.lockupViewModel) return; // nothing playable nests inside an item
    for (const v of Object.values(r)) walk(v);
  };
  walk(data);
  return out;
}

// the token for more of the page itself: section lists and grids, not one shelf's sideways list
export function tvContinuation(data: unknown): string | undefined {
  let found: string | undefined;
  const walk = (o: unknown, inShelf: boolean): void => {
    if (found || !o || typeof o !== "object") return;
    for (const [k, v] of Object.entries(o as Raw)) {
      if (k === "nextContinuationData" && !inShelf && typeof (v as Raw)?.continuation === "string") {
        found = (v as Raw).continuation;
        return;
      }
      walk(v, inShelf || k === "horizontalListRenderer" || k === "horizontalListContinuation");
    }
  };
  walk(data, false);
  return found;
}

export type TvBrowse = (args: Record<string, unknown>) => Promise<unknown>;

// one response as a pager chunk; `more` asks for the next one through the same client
export function tvChunk(browse: TvBrowse, data: unknown, kind: "video" | "playlist" = "video"): Chunk<TvItem> {
  const token = tvContinuation(data);
  return {
    items: tvItems(data).filter((i) => i.kind === kind),
    more: token ? async () => tvChunk(browse, await browse({ continuation: token }), kind) : undefined,
  };
}

// --- the sign-in ---

type Session = Innertube["session"];

export class TvAccount {
  private client: Innertube | null = null;
  private pending: TvPending | null = null;
  private generation = 0; // a newer start or a cancel makes an older code's approval worthless
  private account: TvAccountInfo | null = null;
  private error: "denied" | "expired" | "revoked" | "failed" | null = null;
  onChange: () => void = () => undefined; // signed in or out: browse.ts drops what it cached

  constructor(
    private readonly file: string,
    private readonly log: Log,
    private readonly create: () => Promise<Innertube> = () => Innertube.create({ retrieve_player: false, generate_session_locally: true, lang: "en" }),
  ) {}

  get signedIn(): boolean {
    return this.client !== null;
  }

  state() {
    const pending = this.pending && this.pending.expiresAt > Date.now() ? this.pending : null;
    return { signedIn: this.signedIn, account: this.account, pending, error: this.error };
  }

  // the saved sign-in, at start; a sign-in revoked from the Google account is forgotten
  async load(): Promise<void> {
    if (!existsSync(this.file)) return;
    try {
      const tokens = JSON.parse(await readFile(this.file, "utf8"));
      const yt = await this.create();
      this.watch(yt.session, ++this.generation);
      await yt.session.signIn(tokens);
      this.client = yt;
      this.onChange();
      this.account = await this.who(yt);
      this.log.info({ account: this.account?.handle ?? this.account?.name ?? null }, "code sign-in loaded");
    } catch (err) {
      this.client = null;
      this.error = "revoked";
      this.log.warn({ err: String((err as Error)?.message ?? err).slice(0, 200) }, "the code sign-in no longer works: sign in again on the admin page");
      await rm(this.file, { force: true });
    }
  }

  // a new code for the admin page; the approval arrives later (youtubei.js polls Google)
  async begin(): Promise<TvPending> {
    const gen = ++this.generation;
    this.error = null;
    const yt = await this.create();
    const session = yt.session;
    return new Promise<TvPending>((resolve, reject) => {
      session.on("auth-pending", (d: { verification_url: string; user_code: string; expires_in: number }) => {
        if (gen !== this.generation) return;
        this.pending = { url: d.verification_url, code: d.user_code, expiresAt: Date.now() + d.expires_in * 1000 };
        resolve(this.pending);
      });
      session.on("auth", () => {
        if (gen !== this.generation) {
          void session.signOut().catch(() => undefined); // approved after a cancel: give it back
          return;
        }
        this.pending = null;
        const before = this.client;
        this.client = yt;
        this.onChange();
        void this.save(session);
        // youtubei.js marks the session signed in right after this listener; asked now, the
        // account lookup would go out without the token
        setImmediate(() =>
          void this.who(yt).then((a) => {
            this.account = a;
            this.log.info({ account: a?.handle ?? a?.name ?? null }, "signed in with a code");
          }),
        );
        if (before && before !== yt) void before.session.signOut().catch(() => undefined);
      });
      session.on("auth-error", (err: Error) => {
        if (gen !== this.generation) return;
        this.pending = null;
        const m = String(err?.message ?? "");
        this.error = /denied/i.test(m) ? "denied" : /expired/i.test(m) ? "expired" : "failed";
        reject(err);
      });
      this.watch(session, gen);
      session.signIn().catch(() => undefined); // settles on auth / auth-error, handled above
    });
  }

  cancel(): void {
    this.generation++;
    this.pending = null;
  }

  async signOut(): Promise<void> {
    this.generation++;
    this.pending = null;
    const yt = this.client;
    this.client = null;
    this.account = null;
    this.error = null;
    this.onChange();
    await rm(this.file, { force: true });
    if (yt) await yt.session.signOut().catch(() => undefined); // revoke at Google too
    this.log.info({}, "code sign-in removed");
  }

  // a TV request with the signed-in account
  async browse(args: Record<string, unknown>): Promise<unknown> {
    const yt = this.client;
    if (!yt) throw new Error("not signed in");
    return (await yt.actions.execute("/browse", { ...args, client: "TV" })).data;
  }

  private watch(session: Session, gen: number): void {
    session.on("update-credentials", () => {
      if (gen === this.generation) void this.save(session);
    });
  }

  // tokens plus the client id they belong to (refreshing needs the same one), written atomically
  private async save(session: Session): Promise<void> {
    const tokens = { ...session.oauth.oauth2_tokens, client: session.oauth.client_id };
    await mkdir(dirname(this.file), { recursive: true });
    const tmp = `${this.file}.tmp`;
    await writeFile(tmp, JSON.stringify(tokens), { mode: 0o600 });
    await rename(tmp, this.file);
  }

  private async who(yt: Innertube): Promise<TvAccountInfo | null> {
    try {
      const info = await yt.account.getInfo();
      const item = info.contents?.contents.find((c) => "account_name" in c) as { account_name?: { toString(): string }; channel_handle?: { toString(): string } } | undefined;
      const name = item?.account_name?.toString().trim();
      return name ? { name, handle: item?.channel_handle?.toString().trim() || null } : null;
    } catch {
      return null;
    }
  }
}
