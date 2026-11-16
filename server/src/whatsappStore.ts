// WhatsApp's chats, messages and contacts on the data volume (whatsapp.ts writes them as the linked
// device hears them): one SQLite file, through Node's own node:sqlite. Every write takes the next
// number of a counter (seq), so the Rondo asks only for what changed since the last it saw.

import { DatabaseSync } from "node:sqlite";
import type { Change, WaMessage } from "./whatsappReduce.js";

export interface WaChatRow {
  jid: string;
  name: string; // a group's subject, or what the chat itself is called ("" none)
  group: boolean;
  at: number; // the last message's time (ms)
  unread: number;
  archived: boolean;
  pinned: boolean;
  seq: number;
}

export interface WaContactRow {
  jid: string; // a phone number's jid when known, else a LID
  lid: string;
  pn: string; // the phone number's jid ("" unknown)
  name: string; // saved on the phone
  notify: string; // what they call themselves
  verified: string; // a business's verified name
  about: string;
  aboutAt: number; // when it was asked (ms)
}

export interface WaStoredMessage extends WaMessage {
  reactions: Record<string, string>; // sender -> emoji
  edited: boolean;
  file: string; // the media's file on the volume ("" not downloaded)
  seq: number;
}

const SCHEMA = `
CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS chats (
  jid TEXT PRIMARY KEY, name TEXT NOT NULL DEFAULT '', is_group INTEGER NOT NULL DEFAULT 0, at INTEGER NOT NULL DEFAULT 0,
  unread INTEGER NOT NULL DEFAULT 0, archived INTEGER NOT NULL DEFAULT 0, pinned INTEGER NOT NULL DEFAULT 0, seq INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS chats_seq ON chats(seq);
CREATE TABLE IF NOT EXISTS messages (
  chat TEXT NOT NULL, id TEXT NOT NULL, from_me INTEGER NOT NULL, sender TEXT NOT NULL, sender_name TEXT NOT NULL DEFAULT '',
  at INTEGER NOT NULL, kind TEXT NOT NULL, text TEXT NOT NULL DEFAULT '', media TEXT, thumb TEXT, quoted TEXT,
  status INTEGER NOT NULL DEFAULT 0, reactions TEXT, edited INTEGER NOT NULL DEFAULT 0, file TEXT NOT NULL DEFAULT '',
  raw BLOB, seq INTEGER NOT NULL DEFAULT 0, PRIMARY KEY (chat, id)
);
CREATE INDEX IF NOT EXISTS messages_at ON messages(chat, at);
CREATE INDEX IF NOT EXISTS messages_seq ON messages(seq);
CREATE TABLE IF NOT EXISTS contacts (
  jid TEXT PRIMARY KEY, lid TEXT NOT NULL DEFAULT '', pn TEXT NOT NULL DEFAULT '', name TEXT NOT NULL DEFAULT '',
  notify TEXT NOT NULL DEFAULT '', verified TEXT NOT NULL DEFAULT '', about TEXT NOT NULL DEFAULT '', about_at INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE IF NOT EXISTS lids (lid TEXT PRIMARY KEY, pn TEXT NOT NULL);
`;

type Row = Record<string, unknown>;
const str = (v: unknown) => (typeof v === "string" ? v : "");
const int = (v: unknown) => (typeof v === "number" ? v : typeof v === "bigint" ? Number(v) : 0);

export class WaStore {
  private db: DatabaseSync;
  private seq: number;

  constructor(file: string) {
    this.db = new DatabaseSync(file);
    this.db.exec("PRAGMA journal_mode = WAL; PRAGMA synchronous = NORMAL;");
    this.db.exec(SCHEMA);
    const row = this.db.prepare("SELECT value FROM meta WHERE key = 'seq'").get() as Row | undefined;
    this.seq = row ? Number(row.value) || 0 : 0;
  }

  close(): void {
    this.db.close();
  }

  // the counter's next number, kept (a write takes it)
  private next(): number {
    this.seq += 1;
    this.db.prepare("INSERT INTO meta (key, value) VALUES ('seq', ?) ON CONFLICT(key) DO UPDATE SET value = excluded.value").run(String(this.seq));
    return this.seq;
  }
  latest(): number {
    return this.seq;
  }

  transaction<T>(work: () => T): T {
    this.db.exec("BEGIN");
    try {
      const out = work();
      this.db.exec("COMMIT");
      return out;
    } catch (err) {
      this.db.exec("ROLLBACK");
      throw err;
    }
  }

  meta(key: string): string | undefined {
    const row = this.db.prepare("SELECT value FROM meta WHERE key = ?").get(key) as Row | undefined;
    return row ? str(row.value) : undefined;
  }
  setMeta(key: string, value: string): void {
    this.db.prepare("INSERT INTO meta (key, value) VALUES (?, ?) ON CONFLICT(key) DO UPDATE SET value = excluded.value").run(key, value);
  }

  // ---- LIDs: WhatsApp's private ids, and the phone numbers they stand for ----
  mapLid(lid: string, pn: string): void {
    if (!lid || !pn) return;
    this.db.prepare("INSERT INTO lids (lid, pn) VALUES (?, ?) ON CONFLICT(lid) DO UPDATE SET pn = excluded.pn").run(lid, pn);
  }
  // a private id's number: from WhatsApp's mapping, else from a contact that carries both
  pnOf(lid: string): string {
    const row = this.db.prepare("SELECT pn FROM lids WHERE lid = ?").get(lid) as Row | undefined;
    if (row) return str(row.pn);
    const c = this.db.prepare("SELECT jid FROM contacts WHERE lid = ? AND jid LIKE '%@s.whatsapp.net' LIMIT 1").get(lid) as Row | undefined;
    return c ? str(c.jid) : "";
  }

  // ---- chats ----
  upsertChat(jid: string, c: { name?: string; group?: boolean; at?: number; unread?: number; archived?: boolean; pinned?: boolean }): void {
    const seq = this.next();
    this.db
      .prepare(
        `INSERT INTO chats (jid, name, is_group, at, unread, archived, pinned, seq) VALUES (?, ?, ?, ?, ?, ?, ?, ?)
         ON CONFLICT(jid) DO UPDATE SET
           name = CASE WHEN ?2 != '' THEN ?2 ELSE chats.name END,
           is_group = MAX(chats.is_group, ?3),
           at = MAX(chats.at, ?4),
           unread = CASE WHEN ?9 THEN ?5 ELSE chats.unread END,
           archived = CASE WHEN ?10 THEN ?6 ELSE chats.archived END,
           pinned = CASE WHEN ?11 THEN ?7 ELSE chats.pinned END,
           seq = ?8`,
      )
      .run(
        jid,
        c.name ?? "",
        c.group ?? jid.endsWith("@g.us") ? 1 : 0,
        c.at ?? 0,
        c.unread ?? 0,
        c.archived ? 1 : 0,
        c.pinned ? 1 : 0,
        seq,
        c.unread !== undefined ? 1 : 0,
        c.archived !== undefined ? 1 : 0,
        c.pinned !== undefined ? 1 : 0,
      );
  }
  addUnread(jid: string, by: number): void {
    const seq = this.next();
    this.db.prepare("UPDATE chats SET unread = MAX(0, unread + ?), seq = ? WHERE jid = ?").run(by, seq, jid);
  }
  deleteChat(jid: string): void {
    this.db.prepare("DELETE FROM messages WHERE chat = ?").run(jid);
    this.db.prepare("DELETE FROM chats WHERE jid = ?").run(jid);
    this.next();
  }
  chat(jid: string): WaChatRow | undefined {
    const r = this.db.prepare("SELECT * FROM chats WHERE jid = ?").get(jid) as Row | undefined;
    return r ? chatRow(r) : undefined;
  }
  chats(sinceSeq = 0): WaChatRow[] {
    return (this.db.prepare("SELECT * FROM chats WHERE seq > ? ORDER BY pinned DESC, at DESC").all(sinceSeq) as Row[]).map(chatRow);
  }

  // ---- messages ----
  // a message heard or sent: kept (an older copy's reactions, edit and file stay), its chat's time moved on
  putMessage(m: WaMessage, raw?: Uint8Array): boolean {
    const had = this.db.prepare("SELECT 1 FROM messages WHERE chat = ? AND id = ?").get(m.chat, m.id) !== undefined;
    const seq = this.next();
    this.db
      .prepare(
        `INSERT INTO messages (chat, id, from_me, sender, sender_name, at, kind, text, media, thumb, quoted, status, raw, seq)
         VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
         ON CONFLICT(chat, id) DO UPDATE SET
           sender_name = CASE WHEN excluded.sender_name != '' THEN excluded.sender_name ELSE messages.sender_name END,
           kind = excluded.kind, text = CASE WHEN messages.edited THEN messages.text ELSE excluded.text END,
           media = excluded.media, thumb = COALESCE(excluded.thumb, messages.thumb), quoted = COALESCE(excluded.quoted, messages.quoted),
           status = MAX(messages.status, excluded.status), raw = COALESCE(excluded.raw, messages.raw), seq = excluded.seq`,
      )
      .run(
        m.chat,
        m.id,
        m.fromMe ? 1 : 0,
        m.sender,
        m.senderName,
        m.at,
        m.kind,
        m.text,
        m.media ? JSON.stringify(m.media) : null,
        m.thumb ?? null,
        m.quoted ? JSON.stringify(m.quoted) : null,
        m.status,
        raw ?? null,
        seq,
      );
    this.db.prepare("INSERT INTO chats (jid, is_group, at, seq) VALUES (?, ?, ?, ?) ON CONFLICT(jid) DO UPDATE SET at = MAX(chats.at, excluded.at), seq = excluded.seq").run(
      m.chat,
      m.chat.endsWith("@g.us") ? 1 : 0,
      m.at,
      seq,
    );
    return !had;
  }
  setStatus(chat: string, id: string, status: number): void {
    const seq = this.next();
    this.db.prepare("UPDATE messages SET status = MAX(status, ?), seq = ? WHERE chat = ? AND id = ?").run(status, seq, chat, id);
    this.touchChat(chat, seq);
  }
  // the chat's line in the list follows a change to its messages (ticks, an edit, a deletion)
  private touchChat(chat: string, seq: number): void {
    this.db.prepare("UPDATE chats SET seq = ? WHERE jid = ?").run(seq, chat);
  }
  // others' group messages kept without a sender (with their raw proto, to read it from)
  withoutSender(): { chat: string; id: string; raw: Uint8Array }[] {
    const rows = this.db
      .prepare("SELECT chat, id, raw FROM messages WHERE sender = '' AND from_me = 0 AND chat LIKE '%@g.us' AND raw IS NOT NULL")
      .all() as Row[];
    return rows.map((r) => ({ chat: str(r.chat), id: str(r.id), raw: r.raw as Uint8Array }));
  }
  setSender(chat: string, id: string, sender: string, name: string): void {
    this.db.prepare("UPDATE messages SET sender = ?, sender_name = CASE WHEN ? != '' THEN ? ELSE sender_name END WHERE chat = ? AND id = ?").run(sender, name, name, chat, id);
  }
  setFile(chat: string, id: string, file: string): void {
    this.db.prepare("UPDATE messages SET file = ? WHERE chat = ? AND id = ?").run(file, chat, id);
  }
  apply(c: Change): void {
    if (c.kind === "react" && !c.sender) return; // whose it is unknown: it would count a second time
    const seq = this.next();
    this.touchChat(c.chat, seq);
    if (c.kind === "delete") {
      this.db.prepare("UPDATE messages SET kind = 'deleted', text = '', media = NULL, thumb = NULL, seq = ? WHERE chat = ? AND id = ?").run(seq, c.chat, c.target);
    } else if (c.kind === "edit") {
      this.db.prepare("UPDATE messages SET text = ?, edited = 1, seq = ? WHERE chat = ? AND id = ?").run(c.text, seq, c.chat, c.target);
    } else {
      const r = this.db.prepare("SELECT reactions FROM messages WHERE chat = ? AND id = ?").get(c.chat, c.target) as Row | undefined;
      if (!r) return;
      const all = JSON.parse(str(r.reactions) || "{}") as Record<string, string>;
      if (c.emoji) all[c.sender] = c.emoji;
      else delete all[c.sender];
      this.db.prepare("UPDATE messages SET reactions = ?, seq = ? WHERE chat = ? AND id = ?").run(JSON.stringify(all), seq, c.chat, c.target);
    }
  }
  message(chat: string, id: string): WaStoredMessage | undefined {
    const r = this.db.prepare("SELECT * FROM messages WHERE chat = ? AND id = ?").get(chat, id) as Row | undefined;
    return r ? messageRow(r) : undefined;
  }
  raw(chat: string, id: string): Uint8Array | undefined {
    const r = this.db.prepare("SELECT raw FROM messages WHERE chat = ? AND id = ?").get(chat, id) as Row | undefined;
    return r?.raw instanceof Uint8Array ? r.raw : undefined;
  }
  rawById(id: string): Uint8Array | undefined {
    const r = this.db.prepare("SELECT raw FROM messages WHERE id = ? LIMIT 1").get(id) as Row | undefined;
    return r?.raw instanceof Uint8Array ? r.raw : undefined;
  }
  // a chat's messages before a time (ms; 0 the newest), oldest first
  messages(chat: string, before = 0, limit = 50): WaStoredMessage[] {
    const rows = this.db
      .prepare("SELECT * FROM messages WHERE chat = ? AND (? = 0 OR at < ?) ORDER BY at DESC, rowid DESC LIMIT ?")
      .all(chat, before, before, limit) as Row[];
    return rows.map(messageRow).reverse();
  }
  messagesSince(sinceSeq: number, limit = 300): WaStoredMessage[] {
    return (this.db.prepare("SELECT * FROM messages WHERE seq > ? ORDER BY seq LIMIT ?").all(sinceSeq, limit) as Row[]).map(messageRow);
  }
  lastMessage(chat: string): WaStoredMessage | undefined {
    const r = this.db.prepare("SELECT * FROM messages WHERE chat = ? ORDER BY at DESC, rowid DESC LIMIT 1").get(chat) as Row | undefined;
    return r ? messageRow(r) : undefined;
  }
  unreadKeys(chat: string, limit = 200): { id: string; sender: string }[] {
    const rows = this.db
      .prepare("SELECT id, sender FROM messages WHERE chat = ? AND from_me = 0 ORDER BY at DESC LIMIT ?")
      .all(chat, limit) as Row[];
    return rows.map((r) => ({ id: str(r.id), sender: str(r.sender) }));
  }

  // ---- contacts ----
  upsertContact(c: Partial<WaContactRow> & { jid: string }): void {
    this.db
      .prepare(
        `INSERT INTO contacts (jid, lid, pn, name, notify, verified) VALUES (?, ?, ?, ?, ?, ?)
         ON CONFLICT(jid) DO UPDATE SET
           lid = CASE WHEN excluded.lid != '' THEN excluded.lid ELSE contacts.lid END,
           pn = CASE WHEN excluded.pn != '' THEN excluded.pn ELSE contacts.pn END,
           name = CASE WHEN excluded.name != '' THEN excluded.name ELSE contacts.name END,
           notify = CASE WHEN excluded.notify != '' THEN excluded.notify ELSE contacts.notify END,
           verified = CASE WHEN excluded.verified != '' THEN excluded.verified ELSE contacts.verified END`,
      )
      .run(c.jid, c.lid ?? "", c.pn ?? "", c.name ?? "", c.notify ?? "", c.verified ?? "");
  }
  setAbout(jid: string, about: string): void {
    this.db.prepare("INSERT INTO contacts (jid, about, about_at) VALUES (?, ?, ?) ON CONFLICT(jid) DO UPDATE SET about = excluded.about, about_at = excluded.about_at").run(
      jid,
      about,
      Date.now(),
    );
  }
  contact(jid: string): WaContactRow | undefined {
    const r = this.db.prepare("SELECT * FROM contacts WHERE jid = ?").get(jid) as Row | undefined;
    return r ? contactRow(r) : undefined;
  }
  contacts(): WaContactRow[] {
    return (this.db.prepare("SELECT * FROM contacts").all() as Row[]).map(contactRow);
  }

  // everything (unlinking)
  wipe(): void {
    this.db.exec("DELETE FROM messages; DELETE FROM chats; DELETE FROM contacts; DELETE FROM lids; DELETE FROM meta WHERE key != 'seq';");
    this.next();
  }
}

function chatRow(r: Row): WaChatRow {
  return {
    jid: str(r.jid),
    name: str(r.name),
    group: int(r.is_group) === 1,
    at: int(r.at),
    unread: int(r.unread),
    archived: int(r.archived) === 1,
    pinned: int(r.pinned) === 1,
    seq: int(r.seq),
  };
}

function contactRow(r: Row): WaContactRow {
  return {
    jid: str(r.jid),
    lid: str(r.lid),
    pn: str(r.pn),
    name: str(r.name),
    notify: str(r.notify),
    verified: str(r.verified),
    about: str(r.about),
    aboutAt: int(r.about_at),
  };
}

function messageRow(r: Row): WaStoredMessage {
  const m: WaStoredMessage = {
    chat: str(r.chat),
    id: str(r.id),
    fromMe: int(r.from_me) === 1,
    sender: str(r.sender),
    senderName: str(r.sender_name),
    at: int(r.at),
    kind: str(r.kind) as WaStoredMessage["kind"],
    text: str(r.text),
    status: int(r.status),
    reactions: JSON.parse(str(r.reactions) || "{}") as Record<string, string>,
    edited: int(r.edited) === 1,
    file: str(r.file),
    seq: int(r.seq),
  };
  if (r.media) m.media = JSON.parse(str(r.media));
  if (r.thumb) m.thumb = str(r.thumb);
  if (r.quoted) m.quoted = JSON.parse(str(r.quoted));
  return m;
}
