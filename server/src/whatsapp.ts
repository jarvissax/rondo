// WhatsApp on the Rondo (server mode): this server is a linked device of the owner's WhatsApp, as
// WhatsApp Web is (Baileys, the multi-device protocol). Linked once with a pairing code typed on the
// phone (Linked devices › Link with phone number), it keeps the chats, messages and contacts it hears
// (whatsappStore.ts), saves photos, videos and voice messages on the data volume as they come (an old
// one from the history when first asked), and sends what the Rondo sends. It never shows the owner
// online (the phone keeps its notifications); a chat the Rondo opens is marked read.

import { spawn } from "node:child_process";
import { existsSync, mkdirSync } from "node:fs";
import { mkdir, readFile, rm, stat, writeFile } from "node:fs/promises";
import { join } from "node:path";
import makeWASocket, {
  Browsers,
  DisconnectReason,
  downloadMediaMessage,
  fetchLatestBaileysVersion,
  isJidGroup,
  jidNormalizedUser,
  proto,
  useMultiFileAuthState,
  type WAMessage,
  type WASocket,
} from "baileys";
import sharp from "sharp";
import { WaStore, type WaChatRow, type WaStoredMessage } from "./whatsappStore.js";
import { changeOf, groupSender, previewText, reduceMessage } from "./whatsappReduce.js";

export type WaState = "unlinked" | "linking" | "connecting" | "open" | "closed";

interface Log {
  info: (obj: object | string, msg?: string) => void;
  warn: (obj: object | string, msg?: string) => void;
  error: (obj: object | string, msg?: string) => void;
  child?: (b: object, o?: object) => unknown;
}

export interface WaStatus {
  state: WaState;
  me: { jid: string; name: string; number: string } | null;
  code: { code: string; expiresAt: number } | null; // the pairing code to type on the phone
  error: string;
  syncing: boolean; // the first history still coming
  chats: number;
}

const PAIRING_MS = 160_000; // a pairing code is good for about three minutes
const AVATAR_MS = 7 * 24 * 3600_000;
const ABOUT_MS = 24 * 3600_000;

const extOf = (mime: string): string => {
  const m = mime.split(";")[0]!.trim().toLowerCase();
  if (m === "image/jpeg") return "jpg";
  if (m === "image/png") return "png";
  if (m === "image/webp") return "webp";
  if (m === "video/mp4") return "mp4";
  if (m.startsWith("audio/ogg")) return "ogg";
  if (m === "audio/mpeg") return "mp3";
  if (m === "audio/mp4" || m === "audio/aac") return "m4a";
  if (m === "application/pdf") return "pdf";
  return "bin";
};
const safe = (s: string) => s.replace(/[^A-Za-z0-9._-]/g, "_");
export const numberOf = (jid: string): string => {
  const user = jid.split("@")[0]!.split(":")[0]!;
  return jid.endsWith("@s.whatsapp.net") && /^\d{5,}$/.test(user) ? `+${user}` : "";
};

// a program's output (ffmpeg, ffprobe), its input given
function run(cmd: string, args: string[], input?: Buffer): Promise<Buffer> {
  return new Promise((resolve, reject) => {
    const p = spawn(cmd, args, { stdio: ["pipe", "pipe", "pipe"] });
    const out: Buffer[] = [];
    let err = "";
    p.stdout.on("data", (d: Buffer) => out.push(d));
    p.stderr.on("data", (d: Buffer) => (err = (err + d.toString()).slice(-2000)));
    p.on("error", reject);
    p.on("close", (code) => (code === 0 ? resolve(Buffer.concat(out)) : reject(new Error(`${cmd} ${code}: ${err.trim().split("\n").pop()}`))));
    if (input) p.stdin.end(input);
    else p.stdin.end();
  });
}

// WhatsApp's waveform of a voice message: 64 bars, 0..100, from 16-bit mono PCM
export function waveform(pcm: Int16Array, bars = 64): Uint8Array {
  const out = new Uint8Array(bars);
  if (!pcm.length) return out;
  const per = Math.max(1, Math.floor(pcm.length / bars));
  let peak = 1;
  const levels: number[] = [];
  for (let b = 0; b < bars; b++) {
    let sum = 0;
    const from = b * per, to = Math.min(pcm.length, from + per);
    for (let i = from; i < to; i++) sum += Math.abs(pcm[i]!);
    const level = to > from ? sum / (to - from) : 0;
    levels.push(level);
    peak = Math.max(peak, level);
  }
  for (let b = 0; b < bars; b++) out[b] = Math.round((levels[b]! / peak) * 100);
  return out;
}

export class WhatsApp {
  readonly store: WaStore;
  private sock?: WASocket;
  private state: WaState = "unlinked";
  private error = "";
  private code: { code: string; expiresAt: number; phone: string } | null = null;
  private codeWaiters: ((err?: Error) => void)[] = [];
  private syncing = false;
  private retry = 0;
  private stopped = false;
  private waiters = new Set<() => void>();
  private downloads = new Map<string, Promise<string>>();
  private readonly authDir: string;
  private readonly mediaDir: string;

  constructor(
    private readonly dir: string,
    private readonly log: Log,
    private readonly ffmpeg = "ffmpeg",
  ) {
    this.authDir = join(dir, "auth");
    this.mediaDir = join(dir, "media");
    mkdirSync(dir, { recursive: true, mode: 0o700 });
    this.store = new WaStore(join(dir, "whatsapp.db"));
  }

  // ---- linking ----

  async start(): Promise<void> {
    this.fillSenders();
    if (existsSync(join(this.authDir, "creds.json"))) await this.connect();
  }

  // group messages kept before the sender was read from history's own field (Nov 15): read again
  private fillSenders(): void {
    for (const r of this.store.withoutSender()) {
      const msg = proto.WebMessageInfo.decode(r.raw) as WAMessage;
      const sender = groupSender(msg, (j, alt) => this.resolve(j, alt));
      if (sender) this.store.setSender(r.chat, r.id, sender, msg.pushName ?? "");
    }
  }

  // every group's members, once a connection: their private ids (LIDs) with their numbers, so a
  // member's saved name shows; the groups' names too
  private async learnGroups(sock: WASocket): Promise<void> {
    try {
      const groups = await sock.groupFetchAllParticipating();
      for (const g of Object.values(groups)) {
        if (g.subject) this.store.upsertChat(g.id, { name: g.subject, group: true });
        for (const p of g.participants as { id: string; lid?: string; phoneNumber?: string }[]) {
          if (p.id.endsWith("@lid") && p.phoneNumber) this.store.mapLid(jidNormalizedUser(p.id), jidNormalizedUser(p.phoneNumber));
          else if (p.lid && p.id.endsWith("@s.whatsapp.net")) this.store.mapLid(jidNormalizedUser(p.lid), jidNormalizedUser(p.id));
        }
      }
      this.changed();
    } catch (err) {
      this.log.warn({ err: (err as Error).message }, "whatsapp: the groups' members didn't come");
    }
  }

  stop(): void {
    this.stopped = true;
    this.sock?.end(undefined);
    this.store.close();
  }

  status(): WaStatus {
    const user = this.sock?.user;
    const myJid = user?.id ? jidNormalizedUser(user.id) : "";
    return {
      state: this.state,
      me: this.state === "open" && myJid ? { jid: myJid, name: user?.name ?? user?.notify ?? "", number: numberOf(myJid) } : null,
      code: this.code && this.code.expiresAt > Date.now() && this.state === "linking" ? { code: this.code.code, expiresAt: this.code.expiresAt } : null,
      error: this.error,
      syncing: this.syncing,
      chats: this.store.chats().length,
    };
  }

  // a pairing code for the phone with this number (digits with the country code), a fresh device
  async link(phone: string): Promise<{ code: string; expiresAt: number }> {
    const digits = phone.replace(/\D/g, "");
    if (digits.length < 8 || digits.length > 15) throw new Error("number");
    if (this.state === "open") throw new Error("linked");
    this.sock?.end(undefined);
    this.sock = undefined;
    await rm(this.authDir, { recursive: true, force: true });
    this.code = null;
    this.error = "";
    const got = new Promise<void>((resolve, reject) => {
      const timer = setTimeout(() => reject(new Error("timeout")), 30_000);
      this.codeWaiters.push((err) => {
        clearTimeout(timer);
        if (err) reject(err);
        else resolve();
      });
    });
    await this.connect(digits);
    await got;
    return { code: this.code!.code, expiresAt: this.code!.expiresAt };
  }

  // this server leaves the owner's linked devices; its chats and media go
  async unlink(): Promise<void> {
    try {
      if (this.state === "open") await this.sock?.logout();
    } catch {
      /* gone already */
    }
    this.sock?.end(undefined);
    this.sock = undefined;
    await this.forget();
  }

  private async forget(): Promise<void> {
    this.state = "unlinked";
    this.code = null;
    await rm(this.authDir, { recursive: true, force: true });
    await rm(this.mediaDir, { recursive: true, force: true });
    this.store.wipe();
    this.changed();
  }

  private async connect(pairPhone?: string): Promise<void> {
    await mkdir(this.authDir, { recursive: true, mode: 0o700 });
    const { state, saveCreds } = await useMultiFileAuthState(this.authDir);
    const version = await fetchLatestBaileysVersion()
      .then((v) => v.version)
      .catch(() => undefined);
    const logger = (this.log.child ? this.log.child({ module: "whatsapp" }, { level: "warn" }) : this.log) as never;
    const sock = makeWASocket({
      auth: state,
      ...(version ? { version } : {}),
      // Chrome on a Mac: linking with a phone number fails for a "Desktop" companion (the phone says it
      // couldn't link, Nov 15); Chrome is what that flow is known to work with
      browser: Browsers.macOS("Chrome"),
      markOnlineOnConnect: false, // the phone keeps its notifications
      syncFullHistory: false, // recent chats only, as WhatsApp gives a new device
      logger,
      getMessage: async (key) => {
        const raw = key.id ? this.store.rawById(key.id) : undefined;
        return raw ? (proto.WebMessageInfo.decode(raw).message ?? undefined) : undefined;
      },
    });
    this.sock = sock;
    this.state = pairPhone ? "linking" : "connecting";
    let asked = false;
    sock.ev.on("creds.update", saveCreds);
    sock.ev.process(async (ev) => {
      if (sock !== this.sock) return; // a socket replaced meanwhile
      const u = ev["connection.update"];
      if (u) {
        // the code is asked for once WhatsApp has opened the pairing session (its first QR), not before:
        // asked at "connecting" it reached WhatsApp too early and the phone couldn't link (Nov 15)
        if (pairPhone && !asked && !state.creds.registered && u.qr) {
          asked = true;
          try {
            const code = await sock.requestPairingCode(pairPhone);
            this.code = { code, expiresAt: Date.now() + PAIRING_MS, phone: pairPhone };
            this.codeWaiters.splice(0).forEach((f) => f());
          } catch (err) {
            this.codeWaiters.splice(0).forEach((f) => f(err as Error));
          }
          this.changed();
        }
        if (u.connection === "open") {
          this.state = "open";
          this.code = null;
          this.error = "";
          this.retry = 0;
          if (u.isNewLogin) this.syncing = true;
          this.log.info("whatsapp: connected"); // no number: the log reaches the admin page
          void this.learnGroups(sock);
          this.changed();
        } else if (u.connection === "close") {
          const status = (u.lastDisconnect?.error as { output?: { statusCode?: number } } | undefined)?.output?.statusCode;
          if (status === DisconnectReason.loggedOut) {
            this.log.warn("whatsapp: the phone unlinked this server");
            this.sock = undefined;
            await this.forget();
            this.error = "Unlinked from the phone";
            return;
          }
          if (this.stopped) return;
          // a restart WhatsApp asks for (right after linking), a dropped connection: again, a little later each time
          const linking = this.state === "linking";
          this.state = linking ? "linking" : "connecting";
          const wait = status === DisconnectReason.restartRequired ? 500 : Math.min(60_000, 2000 * 2 ** this.retry++);
          if (!linking || state.creds.registered) setTimeout(() => !this.stopped && void this.connect(), wait);
          else if (this.code && this.code.expiresAt < Date.now()) {
            this.state = "unlinked";
            this.code = null;
          }
          this.changed();
        }
      }
      const hist = ev["messaging-history.set"];
      if (hist) this.takeHistory(hist);
      if (ev["messaging-history.status"]?.status === "complete") (this.syncing = false), this.changed();
      const up = ev["messages.upsert"];
      if (up) for (const m of up.messages) await this.take(m, up.type === "notify");
      for (const u2 of ev["messages.update"] ?? []) {
        const chat = this.resolve(u2.key.remoteJid, (u2.key as { remoteJidAlt?: string }).remoteJidAlt);
        if (u2.key.id && typeof u2.update.status === "number") this.store.setStatus(chat, u2.key.id, u2.update.status);
        if (u2.key.id && u2.update.message) {
          const ch = changeOf({ key: u2.key, message: u2.update.message } as WAMessage, chat, "");
          if (ch) this.store.apply(ch);
        }
      }
      const del = ev["messages.delete"];
      if (del && "keys" in del)
        for (const k of del.keys) if (k.id) this.store.apply({ kind: "delete", chat: this.resolve(k.remoteJid), target: k.id });
      // reactions come as messages too (take: changeOf), which say who sent them; Baileys'
      // "messages.reaction" repeats them without that, so it isn't read
      for (const c of [...(ev["chats.upsert"] ?? []), ...(ev["chats.update"] ?? [])]) this.takeChat(c);
      for (const jid of ev["chats.delete"] ?? []) this.store.deleteChat(this.resolve(jid));
      for (const c of [...(ev["contacts.upsert"] ?? []), ...(ev["contacts.update"] ?? [])]) this.takeContact(c);
      for (const g of [...(ev["groups.upsert"] ?? []), ...(ev["groups.update"] ?? [])]) if (g.id && g.subject) this.store.upsertChat(g.id, { name: g.subject, group: true });
      const lid = ev["lid-mapping.update"];
      if (lid?.lid && lid.pn) this.store.mapLid(jidNormalizedUser(lid.lid), jidNormalizedUser(lid.pn));
      if (Object.keys(ev).some((k) => k !== "creds.update")) this.changed();
    });
  }

  // ---- what WhatsApp tells this device ----

  private myJid(): string {
    return this.sock?.user?.id ? jidNormalizedUser(this.sock.user.id) : "";
  }

  // a person's chat by their number when it's known (a LID only when it isn't); a group's as it is
  resolve(jid?: string | null, alt?: string | null): string {
    if (!jid) return "";
    const j = jidNormalizedUser(jid);
    if (!j.endsWith("@lid")) return j;
    if (alt && alt.endsWith("@s.whatsapp.net")) {
      const pn = jidNormalizedUser(alt);
      this.store.mapLid(j, pn);
      return pn;
    }
    return this.store.pnOf(j) || j;
  }

  private takeHistory(h: { chats: { id?: string | null; name?: string | null; unreadCount?: number | null; archived?: boolean | null; pinned?: number | null; conversationTimestamp?: unknown }[]; contacts: { id: string; lid?: string; phoneNumber?: string; name?: string; notify?: string; verifiedName?: string }[]; messages: WAMessage[]; lidPnMappings?: { lid: string; pn: string }[] }): void {
    this.store.transaction(() => {
      for (const m of h.lidPnMappings ?? []) this.store.mapLid(jidNormalizedUser(m.lid), jidNormalizedUser(m.pn));
      for (const c of h.contacts) this.takeContact(c);
      for (const c of h.chats) this.takeChat(c);
      for (const m of h.messages) void this.take(m, false);
    });
  }

  private takeChat(c: { id?: string | null; name?: string | null; unreadCount?: number | null; archived?: boolean | null; pinned?: number | null; conversationTimestamp?: unknown }): void {
    if (!c.id) return;
    const jid = this.resolve(c.id);
    if (!jid || jid === "status@broadcast" || jid.endsWith("@newsletter") || jid.endsWith("@broadcast")) return;
    const ts = Number(c.conversationTimestamp ?? 0);
    this.store.upsertChat(jid, {
      name: isJidGroup(jid) ? (c.name ?? "") : "",
      group: !!isJidGroup(jid),
      ...(ts > 0 ? { at: ts * 1000 } : {}),
      ...(typeof c.unreadCount === "number" ? { unread: Math.max(0, c.unreadCount) } : {}),
      ...(typeof c.archived === "boolean" ? { archived: c.archived } : {}),
      ...(c.pinned !== undefined && c.pinned !== null ? { pinned: c.pinned > 0 } : {}),
    });
  }

  private takeContact(c: { id?: string; lid?: string; phoneNumber?: string; name?: string; notify?: string; verifiedName?: string }): void {
    if (!c.id) return;
    const lid = c.lid ? jidNormalizedUser(c.lid) : c.id.endsWith("@lid") ? jidNormalizedUser(c.id) : "";
    const pn = c.phoneNumber ? jidNormalizedUser(c.phoneNumber) : c.id.endsWith("@s.whatsapp.net") ? jidNormalizedUser(c.id) : "";
    if (lid && pn) this.store.mapLid(lid, pn);
    const jid = pn || this.resolve(c.id);
    if (!jid || isJidGroup(jid)) return;
    this.store.upsertContact({ jid, lid, pn, name: c.name ?? "", notify: c.notify ?? "", verified: c.verifiedName ?? "" });
  }

  private async take(msg: WAMessage, live: boolean): Promise<void> {
    const me = this.myJid();
    const m = reduceMessage(msg, (j, alt) => this.resolve(j, alt), me);
    if (!m) {
      const chat = this.resolve(msg.key?.remoteJid, (msg.key as { remoteJidAlt?: string } | undefined)?.remoteJidAlt);
      const sender = msg.key?.fromMe ? me : chat.endsWith("@g.us") ? groupSender(msg, (j, alt) => this.resolve(j, alt)) : chat;
      const ch = chat ? changeOf(msg, chat, sender) : undefined;
      if (ch) this.store.apply(ch);
      return;
    }
    const raw = proto.WebMessageInfo.encode(msg).finish();
    const isNew = this.store.putMessage(m, raw);
    // the history's messages carry their reactions with them
    for (const r of msg.reactions ?? []) {
      const by = r.key?.fromMe ? me : m.chat.endsWith("@g.us") ? this.resolve(r.key?.participant) : m.chat;
      if (by && r.text) this.store.apply({ kind: "react", chat: m.chat, target: m.id, sender: by, emoji: r.text });
    }
    if (msg.pushName && !m.fromMe && m.sender) this.store.upsertContact({ jid: m.sender, notify: msg.pushName });
    if (live && isNew && !m.fromMe) this.store.addUnread(m.chat, 1);
    // what's heard as it happens is saved at once; the history's media when first asked for
    if (live && m.media && m.kind !== "document") void this.mediaFile(m.chat, m.id).catch(() => undefined);
  }

  // ---- the Rondo's waits ----

  private changed(): void {
    for (const w of [...this.waiters]) w();
  }

  // resolves once anything changed after `since` (or after ms)
  waitFor(since: number, ms: number): Promise<void> {
    if (this.store.latest() > since) return Promise.resolve();
    return new Promise((resolve) => {
      const done = () => {
        clearTimeout(timer);
        this.waiters.delete(done);
        resolve();
      };
      const timer = setTimeout(done, ms);
      this.waiters.add(done);
    });
  }

  // ---- names and profiles ----

  // a person's name as the phone shows it: saved, a business's, what they call themselves ("~Name");
  // a group member known only by a private id (a LID, history's messages) is found by its number when
  // WhatsApp mapped it, else by the name their message carried (pushName)
  nameOf(jid: string, pushName = ""): string {
    const pn = jid.endsWith("@lid") ? this.store.pnOf(jid) : "";
    for (const j of pn ? [pn, jid] : [jid]) {
      const c = this.store.contact(j);
      if (c?.name) return c.name;
      const chat = this.store.chat(j);
      if (chat?.name) return chat.name;
    }
    for (const j of pn ? [pn, jid] : [jid]) {
      const c = this.store.contact(j);
      if (c?.verified) return c.verified;
      if (c?.notify) return `~${c.notify}`;
    }
    if (pushName) return `~${pushName}`;
    if (jid === this.myJid()) return "You";
    return numberOf(pn || jid) || "Someone";
  }

  async about(jid: string): Promise<string> {
    const c = this.store.contact(jid);
    if (c && c.aboutAt > Date.now() - ABOUT_MS) return c.about;
    try {
      const res = (await this.sock?.fetchStatus(jid)) as { status?: { status?: string } }[] | undefined;
      const about = res?.[0]?.status?.status ?? "";
      this.store.setAbout(jid, about);
      return about;
    } catch {
      return c?.about ?? "";
    }
  }

  // a person's or a group's picture, kept a week; "" when they have none (or hide it)
  async avatarFile(jid: string): Promise<string> {
    const file = join(this.mediaDir, "avatars", `${safe(jid)}.jpg`);
    const none = `${file}.none`;
    const fresh = async (p: string) => (await stat(p).catch(() => undefined))?.mtimeMs ?? 0;
    if ((await fresh(file)) > Date.now() - AVATAR_MS) return file;
    if ((await fresh(none)) > Date.now() - AVATAR_MS) return "";
    await mkdir(join(this.mediaDir, "avatars"), { recursive: true });
    try {
      const url = await this.sock?.profilePictureUrl(jid, "image");
      if (!url) throw new Error("none");
      const res = await fetch(url);
      if (!res.ok) throw new Error(String(res.status));
      await writeFile(file, Buffer.from(await res.arrayBuffer()));
      return file;
    } catch {
      await writeFile(none, "");
      return existsSync(file) ? file : "";
    }
  }

  // ---- media ----

  // a message's photo, video, voice, sticker or document on the volume, downloaded once
  async mediaFile(chat: string, id: string): Promise<string> {
    const key = `${chat}/${id}`;
    const held = this.downloads.get(key);
    if (held) return held;
    const work = (async () => {
      const m = this.store.message(chat, id);
      if (!m?.media) throw new Error("not media");
      if (m.file && existsSync(m.file)) return m.file;
      const raw = this.store.raw(chat, id);
      if (!raw || !this.sock) throw new Error("unavailable");
      const msg = proto.WebMessageInfo.decode(raw) as WAMessage;
      const buf = await downloadMediaMessage(msg, "buffer", {}, { logger: this.log as never, reuploadRequest: this.sock.updateMediaMessage });
      const dir = join(this.mediaDir, safe(chat));
      await mkdir(dir, { recursive: true });
      const file = join(dir, `${safe(id)}.${extOf(m.media.mime)}`);
      await writeFile(file, buf);
      this.store.setFile(chat, id, file);
      return file;
    })();
    this.downloads.set(key, work);
    try {
      return await work;
    } finally {
      this.downloads.delete(key);
    }
  }

  // a video as the Y1 plays it: H.264 and AAC in MP4, at most 854 x 480 (made once, kept beside it)
  async videoForY1(chat: string, id: string): Promise<string> {
    const file = await this.mediaFile(chat, id);
    const out = file.replace(/\.[^.]+$/, ".y1.mp4");
    if (existsSync(out)) return out;
    let fits = false;
    try {
      const probe = (await run("ffprobe", ["-v", "error", "-select_streams", "v:0", "-show_entries", "stream=codec_name,width,height", "-of", "csv=p=0", file])).toString().trim();
      const [codec, w, h] = probe.split(",");
      fits = codec === "h264" && Number(w) <= 1280 && Number(h) <= 720;
    } catch {
      fits = false;
    }
    if (fits) return file;
    await run(this.ffmpeg, ["-v", "error", "-y", "-i", file, "-vf", "scale='min(854,iw)':-2", "-c:v", "libx264", "-profile:v", "baseline", "-preset", "veryfast", "-crf", "26", "-c:a", "aac", "-b:a", "96k", "-movflags", "+faststart", out]);
    return out;
  }

  // a picture (photo, sticker, avatar) as a JPEG or PNG at most w wide
  async picture(file: string, w: number, png = false): Promise<Buffer> {
    return this.pictureOf(await readFile(file), w, png);
  }

  // a picture's bytes at most w wide and tall (never enlarged), as JPEG or PNG
  async pictureOf(bytes: Buffer, w: number, png = false): Promise<Buffer> {
    const img = sharp(bytes, { animated: false }).rotate().resize({ width: w, height: w, fit: "inside", withoutEnlargement: true });
    return png ? img.png().toBuffer() : img.jpeg({ quality: 82 }).toBuffer();
  }

  // ---- what the Rondo sends ----

  private sockOrThrow(): WASocket {
    if (!this.sock || this.state !== "open") throw new Error("offline");
    return this.sock;
  }

  private quotedOf(chat: string, quoted?: string): WAMessage | undefined {
    if (!quoted) return undefined;
    const raw = this.store.raw(chat, quoted);
    return raw ? (proto.WebMessageInfo.decode(raw) as WAMessage) : undefined;
  }

  // a reaction to a message, as the owner ("" takes it back); the message's key as WhatsApp gave it
  async react(chat: string, id: string, emoji: string): Promise<WaStoredMessage | undefined> {
    const sock = this.sockOrThrow();
    const m = this.store.message(chat, id);
    if (!m) return undefined;
    const raw = this.store.raw(chat, id);
    const key = raw
      ? (proto.WebMessageInfo.decode(raw).key as WAMessage["key"])
      : { remoteJid: chat, id, fromMe: m.fromMe, ...(chat.endsWith("@g.us") && !m.fromMe ? { participant: m.sender } : {}) };
    await sock.sendMessage(chat, { react: { text: emoji, key } });
    this.store.apply({ kind: "react", chat, target: id, sender: this.myJid(), emoji });
    this.changed();
    return this.store.message(chat, id);
  }

  async sendText(chat: string, text: string, quoted?: string): Promise<WaStoredMessage | undefined> {
    const sent = await this.sockOrThrow().sendMessage(chat, { text }, { quoted: this.quotedOf(chat, quoted) });
    return sent ? this.kept(sent) : undefined;
  }

  // a voice message the Y1 recorded (WAV, 16-bit mono): Ogg Opus as WhatsApp's own, with its waveform
  async sendVoice(chat: string, wav: Buffer): Promise<WaStoredMessage | undefined> {
    const sock = this.sockOrThrow();
    const pcm = await run(this.ffmpeg, ["-v", "error", "-i", "pipe:0", "-ac", "1", "-ar", "16000", "-f", "s16le", "pipe:1"], wav);
    const samples = new Int16Array(pcm.buffer, pcm.byteOffset, Math.floor(pcm.length / 2));
    const seconds = Math.max(1, Math.round(samples.length / 16000));
    const ogg = await run(this.ffmpeg, ["-v", "error", "-f", "s16le", "-ar", "16000", "-ac", "1", "-i", "pipe:0", "-c:a", "libopus", "-b:a", "24k", "-application", "voip", "-ar", "48000", "-f", "ogg", "pipe:1"], pcm);
    const sent = await sock.sendMessage(chat, { audio: ogg, mimetype: "audio/ogg; codecs=opus", ptt: true, seconds, waveform: waveform(samples) } as never);
    if (!sent) return undefined;
    const kept = this.kept(sent);
    if (kept) {
      const dir = join(this.mediaDir, safe(chat));
      await mkdir(dir, { recursive: true });
      const file = join(dir, `${safe(kept.id)}.ogg`);
      await writeFile(file, ogg);
      this.store.setFile(chat, kept.id, file);
    }
    return kept;
  }

  // a photo or a video from the Y1's card
  async sendMedia(chat: string, kind: "image" | "video", data: Buffer, caption: string): Promise<WaStoredMessage | undefined> {
    const sock = this.sockOrThrow();
    let body = data;
    if (kind === "image") body = await sharp(data).rotate().resize({ width: 1600, height: 1600, fit: "inside", withoutEnlargement: true }).jpeg({ quality: 82 }).toBuffer();
    const content = kind === "image" ? { image: body, caption: caption || undefined } : { video: body, caption: caption || undefined, mimetype: "video/mp4" };
    const sent = await sock.sendMessage(chat, content as never);
    if (!sent) return undefined;
    const kept = this.kept(sent);
    if (kept) {
      const dir = join(this.mediaDir, safe(chat));
      await mkdir(dir, { recursive: true });
      const file = join(dir, `${safe(kept.id)}.${kind === "image" ? "jpg" : "mp4"}`);
      await writeFile(file, body);
      this.store.setFile(chat, kept.id, file);
    }
    return kept;
  }

  // a message this server sent: kept at once (the echo WhatsApp sends back finds it there)
  private kept(sent: WAMessage): WaStoredMessage | undefined {
    const m = reduceMessage(sent, (j, alt) => this.resolve(j, alt), this.myJid());
    if (!m) return undefined;
    m.status = Math.max(m.status, 2); // WhatsApp took it: sent (one tick), not waiting
    this.store.putMessage(m, proto.WebMessageInfo.encode(sent).finish());
    this.changed();
    return this.store.message(m.chat, m.id);
  }

  // a chat read on the Rondo: read everywhere (blue ticks, when the owner has them)
  async markRead(chat: string): Promise<void> {
    const row = this.store.chat(chat);
    this.store.upsertChat(chat, { unread: 0 });
    this.changed();
    if (!row?.unread || !this.sock || this.state !== "open") return;
    const keys = this.store
      .unreadKeys(chat, Math.min(row.unread, 200))
      .map((k) => ({ remoteJid: chat, id: k.id, fromMe: false, participant: chat.endsWith("@g.us") ? k.sender : undefined }));
    await this.sock.readMessages(keys).catch(() => undefined);
  }

  // ---- the Rondo's JSON ----

  chatJson(c: WaChatRow): object {
    const last = this.store.lastMessage(c.jid);
    return {
      jid: c.jid,
      name: c.group ? c.name || "Group" : this.nameOf(c.jid),
      group: c.group,
      at: c.at,
      unread: c.unread,
      pinned: c.pinned,
      archived: c.archived,
      preview: last
        ? {
            text: previewText(last.kind, last.text, last.media),
            fromMe: last.fromMe,
            sender: c.group && !last.fromMe ? this.nameOf(last.sender, last.senderName) : "",
            status: last.status,
            // what it is, so the list can draw it as WhatsApp does (a mic and 0:03 for a voice message)
            kind: last.kind,
            caption: last.kind === "image" || last.kind === "video" ? last.text : "",
            seconds: last.media?.seconds ?? 0,
          }
        : null,
    };
  }

  messageJson(m: WaStoredMessage): object {
    // a person once, whatever form their id was heard in (their number, or a private id mapped to it);
    // none without one (kept before Nov 16)
    const me = this.myJid();
    const byPerson = new Map<string, string>();
    for (const [who, emoji] of Object.entries(m.reactions)) {
      if (!who || !emoji) continue;
      byPerson.set(who.endsWith("@lid") ? this.store.pnOf(who) || who : who, emoji);
    }
    const counts = new Map<string, number>();
    for (const emoji of byPerson.values()) counts.set(emoji, (counts.get(emoji) ?? 0) + 1);
    return {
      id: m.id,
      fromMe: m.fromMe,
      sender: m.sender,
      name: m.fromMe ? "You" : m.chat.endsWith("@g.us") ? this.nameOf(m.sender, m.senderName) : "",
      at: m.at,
      kind: m.kind,
      text: m.text,
      ...(m.media ? { media: m.media } : {}),
      // the small preview picture: from /wa/media/<chat>/<id>?preview=1, not in the JSON
      ...(m.thumb ? { preview: true } : {}),
      ...(m.quoted ? { quoted: { ...m.quoted, name: m.quoted.sender ? (m.quoted.sender === this.myJid() ? "You" : this.nameOf(m.quoted.sender)) : "" } } : {}),
      status: m.status,
      edited: m.edited,
      reactions: [...counts].map(([emoji, count]) => ({ emoji, count })),
      mine: (me && byPerson.get(me)) || "", // the owner's own reaction ("" none)
      seq: m.seq,
    };
  }

  contactJson(jid: string, about?: string): object {
    const c = this.store.contact(jid);
    return {
      jid,
      name: this.nameOf(jid),
      saved: !!c?.name,
      pushName: c?.notify ?? "",
      number: numberOf(jid),
      ...(about !== undefined ? { about } : {}),
      group: !!isJidGroup(jid),
    };
  }
}
