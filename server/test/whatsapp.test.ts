import { mkdtempSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { afterEach, describe, expect, it } from "vitest";
import type { WAMessage } from "baileys";
import { numberOf, waveform, WhatsApp } from "../src/whatsapp.js";
import { withLang } from "../src/lang.js";
import { changeOf, nameless, previewText, reduceMessage, type JidResolver } from "../src/whatsappReduce.js";
import { WaStore } from "../src/whatsappStore.js";

const ME = "15550000001@s.whatsapp.net";
const ALI = "15550000002@s.whatsapp.net";
const GROUP = "120363000000000001@g.us";
const resolve: JidResolver = (jid, alt) => {
  if (!jid) return "";
  const j = jid.replace(/:\d+@/, "@");
  if (j.endsWith("@lid")) return alt ?? j;
  return j;
};
const msg = (m: object) => m as WAMessage;

describe("WhatsApp messages as the Rondo shows them", () => {
  it("reduces a text, its sender and time", () => {
    const m = reduceMessage(msg({ key: { remoteJid: ALI, id: "A1", fromMe: false }, messageTimestamp: 1794700000, pushName: "Ali", message: { conversation: "Salam!" } }), resolve, ME);
    expect(m).toEqual({ chat: ALI, id: "A1", fromMe: false, sender: ALI, senderName: "Ali", at: 1794700000000, kind: "text", text: "Salam!", status: 0 });
  });

  it("keys a chat by the phone number when WhatsApp gives it beside a LID", () => {
    const m = reduceMessage(msg({ key: { remoteJid: "99999@lid", remoteJidAlt: ALI, id: "A2", fromMe: false }, messageTimestamp: 1, message: { conversation: "hi" } }), resolve, ME);
    expect(m?.chat).toBe(ALI);
  });

  it("reads a photo's caption and facts, and what it quotes", () => {
    const m = reduceMessage(
      msg({
        key: { remoteJid: GROUP, participant: ALI, id: "P1" },
        messageTimestamp: 2,
        message: {
          imageMessage: {
            caption: "Baku",
            mimetype: "image/jpeg",
            width: 1600,
            height: 1200,
            fileLength: 250000,
            jpegThumbnail: new Uint8Array([1, 2, 3]),
            contextInfo: { stanzaId: "A1", participant: ME, quotedMessage: { conversation: "Send a photo" } },
          },
        },
      }),
      resolve,
      ME,
    );
    expect(m).toMatchObject({ chat: GROUP, sender: ALI, kind: "image", text: "Baku", media: { mime: "image/jpeg", width: 1600, height: 1200, size: 250000 }, thumb: "AQID" });
    expect(m?.quoted).toEqual({ id: "A1", sender: ME, kind: "text", text: "Send a photo", raw: true });
  });

  it("makes its own words in the Rondo's language: previews, quotes, nameless contacts and polls", () => {
    expect(previewText("image", "Baku")).toBe("Photo: Baku");
    expect(withLang("es", () => previewText("image", "Baku"))).toBe("Foto: Baku");
    expect(withLang("es", () => previewText("voice", "", { seconds: 72 }))).toBe("Mensaje de voz 1:12");
    expect(withLang("es", () => previewText("document", "", {}))).toBe("Documento: archivo");
    expect(previewText("contact", "\u{1F464} ")).toBe("\u{1F464} Contact");
    expect(withLang("es", () => nameless("poll", "\u{1F4CA} \n• Sí\n• No"))).toBe("\u{1F4CA} Encuesta\n• Sí\n• No");
    expect(nameless("poll", "\u{1F4CA} Lunch?\n• Yes")).toBe("\u{1F4CA} Lunch?\n• Yes");
    // a quote keeps the quoted message's own words; its preview is made for the Rondo asking
    const q = reduceMessage(
      msg({
        key: { remoteJid: ALI, id: "Q1" },
        messageTimestamp: 4,
        message: { extendedTextMessage: { text: "Nice", contextInfo: { stanzaId: "V9", participant: ALI, quotedMessage: { audioMessage: { ptt: true, seconds: 5 } } } } },
      }),
      resolve,
      ME,
    );
    expect(q?.quoted).toMatchObject({ kind: "voice", text: "", raw: true, seconds: 5 });
    expect(withLang("es", () => previewText(q!.quoted!.kind, q!.quoted!.text, q!.quoted))).toBe("Mensaje de voz 0:05");
  });

  it("tells a voice message from an audio file, and gives their length", () => {
    const voice = reduceMessage(msg({ key: { remoteJid: ALI, id: "V1" }, messageTimestamp: 3, message: { audioMessage: { ptt: true, seconds: 12, mimetype: "audio/ogg; codecs=opus" } } }), resolve, ME);
    expect(voice).toMatchObject({ kind: "voice", media: { seconds: 12 } });
    expect(previewText("voice", "", voice?.media)).toBe("Voice message 0:12");
    const audio = reduceMessage(msg({ key: { remoteJid: ALI, id: "V2" }, messageTimestamp: 3, message: { audioMessage: { seconds: 200 } } }), resolve, ME);
    expect(audio?.kind).toBe("audio");
  });

  it("unwraps view-once and disappearing messages", () => {
    const m = reduceMessage(
      msg({ key: { remoteJid: ALI, id: "E1" }, messageTimestamp: 4, message: { ephemeralMessage: { message: { extendedTextMessage: { text: "gone soon" } } } } }),
      resolve,
      ME,
    );
    expect(m).toMatchObject({ kind: "text", text: "gone soon" });
  });

  it("marks my own messages with their status", () => {
    const m = reduceMessage(msg({ key: { remoteJid: ALI, id: "M1", fromMe: true }, messageTimestamp: 5, status: 3, message: { conversation: "ok" } }), resolve, ME);
    expect(m).toMatchObject({ fromMe: true, sender: ME, status: 3 });
  });

  it("leaves out reactions, edits, deletions and status updates as messages of their own", () => {
    const reaction = msg({ key: { remoteJid: ALI, id: "R1" }, messageTimestamp: 6, message: { reactionMessage: { key: { id: "A1" }, text: "❤️" } } });
    expect(reduceMessage(reaction, resolve, ME)).toBeUndefined();
    expect(changeOf(reaction, ALI, ALI)).toEqual({ kind: "react", chat: ALI, target: "A1", sender: ALI, emoji: "❤️" });
    const revoke = msg({ key: { remoteJid: ALI, id: "D1" }, message: { protocolMessage: { type: 0, key: { id: "A1" } } } });
    expect(changeOf(revoke, ALI, ALI)).toEqual({ kind: "delete", chat: ALI, target: "A1" });
    const edit = msg({ key: { remoteJid: ALI, id: "X1" }, message: { protocolMessage: { type: 14, key: { id: "A1" }, editedMessage: { conversation: "Salam!!" } } } });
    expect(changeOf(edit, ALI, ALI)).toEqual({ kind: "edit", chat: ALI, target: "A1", text: "Salam!!" });
    expect(reduceMessage(msg({ key: { remoteJid: "status@broadcast", id: "S1" }, message: { conversation: "story" } }), resolve, ME)).toBeUndefined();
  });

  it("says what it can't show", () => {
    const m = reduceMessage(msg({ key: { remoteJid: ALI, id: "U1" }, messageTimestamp: 7, message: { eventMessage: { name: "Party" } } }), resolve, ME);
    expect(m?.kind).toBe("unsupported");
  });

  it("names a number and draws a voice message's bars", () => {
    expect(numberOf(ALI)).toBe("+15550000002");
    expect(numberOf(GROUP)).toBe("");
    const pcm = new Int16Array(64 * 100);
    for (let i = 0; i < pcm.length; i++) pcm[i] = i < 3200 ? 1000 : 4000;
    const w = waveform(pcm);
    expect(w).toHaveLength(64);
    expect(w[0]).toBe(25);
    expect(w[63]).toBe(100);
  });
});

describe("the WhatsApp store", () => {
  let dir = "";
  afterEach(() => dir && rmSync(dir, { recursive: true, force: true }));
  const open = () => {
    dir = mkdtempSync(join(tmpdir(), "wa-"));
    return new WaStore(join(dir, "wa.db"));
  };

  it("keeps messages in time order and moves its chat's time on", () => {
    const s = open();
    s.putMessage({ chat: ALI, id: "1", fromMe: false, sender: ALI, senderName: "", at: 1000, kind: "text", text: "a", status: 0 });
    s.putMessage({ chat: ALI, id: "2", fromMe: true, sender: ME, senderName: "", at: 3000, kind: "text", text: "b", status: 2 });
    s.putMessage({ chat: ALI, id: "3", fromMe: false, sender: ALI, senderName: "", at: 2000, kind: "text", text: "c", status: 0 });
    expect(s.messages(ALI).map((m) => m.id)).toEqual(["1", "3", "2"]);
    expect(s.messages(ALI, 3000, 1).map((m) => m.id)).toEqual(["3"]);
    expect(s.chat(ALI)?.at).toBe(3000);
  });

  it("applies reactions, edits and deletions, and status only forward", () => {
    const s = open();
    s.putMessage({ chat: ALI, id: "1", fromMe: true, sender: ME, senderName: "", at: 1000, kind: "text", text: "hi", status: 2 });
    s.apply({ kind: "react", chat: ALI, target: "1", sender: ALI, emoji: "\u{1F44D}" });
    s.apply({ kind: "edit", chat: ALI, target: "1", text: "hello" });
    s.setStatus(ALI, "1", 4);
    s.setStatus(ALI, "1", 3);
    let m = s.message(ALI, "1")!;
    expect(m).toMatchObject({ text: "hello", edited: true, status: 4, reactions: { [ALI]: "\u{1F44D}" } });
    // the echo of a message edited meanwhile keeps the edit
    s.putMessage({ chat: ALI, id: "1", fromMe: true, sender: ME, senderName: "", at: 1000, kind: "text", text: "hi", status: 2 });
    expect(s.message(ALI, "1")?.text).toBe("hello");
    s.apply({ kind: "delete", chat: ALI, target: "1" });
    m = s.message(ALI, "1")!;
    expect(m.kind).toBe("deleted");
  });

  it("counts what changed, so the Rondo asks only for that", () => {
    const s = open();
    s.putMessage({ chat: ALI, id: "1", fromMe: false, sender: ALI, senderName: "", at: 1000, kind: "text", text: "a", status: 0 });
    const seen = s.latest();
    s.putMessage({ chat: GROUP, id: "9", fromMe: false, sender: ALI, senderName: "Ali", at: 2000, kind: "text", text: "b", status: 0 });
    s.upsertChat(GROUP, { name: "Family", unread: 1 });
    expect(s.messagesSince(seen).map((m) => m.id)).toEqual(["9"]);
    expect(s.chats(seen).map((c) => c.jid)).toEqual([GROUP]);
    expect(s.chat(GROUP)).toMatchObject({ name: "Family", group: true, unread: 1 });
  });

  it("maps LIDs to numbers and keeps contacts' names", () => {
    const s = open();
    s.mapLid("99999@lid", ALI);
    expect(s.pnOf("99999@lid")).toBe(ALI);
    s.upsertContact({ jid: ALI, name: "Ali Morgan" });
    s.upsertContact({ jid: ALI, notify: "Ali" });
    expect(s.contact(ALI)).toMatchObject({ name: "Ali Morgan", notify: "Ali" });
    s.wipe();
    expect(s.contacts()).toEqual([]);
  });

  it("reads a group message's sender from the message itself (history)", () => {
    const m = reduceMessage(
      { key: { remoteJid: "120363000000000001@g.us", fromMe: false, id: "H1" }, participant: "12345@lid", messageTimestamp: 1, message: { conversation: "hi" } } as never,
      (j) => j ?? "",
      "me@s.whatsapp.net",
    );
    expect(m?.sender).toBe("12345@lid");
  });

  it("lists a chat again when one of its messages is delivered", () => {
    const s = new WaStore(":memory:");
    s.upsertChat("1@s.whatsapp.net", { at: 1 });
    s.putMessage({ chat: "1@s.whatsapp.net", id: "M1", fromMe: true, sender: "me", senderName: "", at: 1, kind: "text", text: "hi", status: 2 });
    const before = s.latest();
    s.setStatus("1@s.whatsapp.net", "M1", 3);
    expect(s.chats(before).map((c) => c.jid)).toEqual(["1@s.whatsapp.net"]);
    expect(s.message("1@s.whatsapp.net", "M1")?.status).toBe(3);
  });

  it("keeps no reaction whose sender is unknown", () => {
    const s = new WaStore(":memory:");
    s.upsertChat("1@s.whatsapp.net", { at: 1 });
    s.putMessage({ chat: "1@s.whatsapp.net", id: "M1", fromMe: true, sender: "me", senderName: "", at: 1, kind: "text", text: "hi", status: 2 });
    s.apply({ kind: "react", chat: "1@s.whatsapp.net", target: "M1", sender: "1@s.whatsapp.net", emoji: "\u2764\ufe0f" });
    s.apply({ kind: "react", chat: "1@s.whatsapp.net", target: "M1", sender: "", emoji: "\u2764\ufe0f" });
    expect(Object.keys(s.message("1@s.whatsapp.net", "M1")!.reactions)).toEqual(["1@s.whatsapp.net"]);
  });
});

describe("WhatsApp's chats and mentions", () => {
  const quiet = { info() {}, warn() {}, error() {}, debug() {} } as never;
  const dirs: string[] = [];
  const open = () => {
    const dir = mkdtempSync(join(tmpdir(), "rondo-wa-"));
    dirs.push(dir);
    return new WhatsApp(dir, quiet);
  };
  afterEach(() => {
    for (const d of dirs.splice(0)) rmSync(d, { recursive: true, force: true });
  });
  const live = (id: string) => msg({ key: { remoteJid: ALI, fromMe: false, id }, messageTimestamp: 1, message: { conversation: "hi" } });

  it("counts each new message once, as a chat update's unreadCount is a step (Baileys)", async () => {
    const wa = open();
    const w = wa as unknown as { take(m: WAMessage, live: boolean): Promise<void>; takeChat(c: object, update?: boolean): void };
    wa.store.upsertChat(ALI, { at: 1 });
    for (const id of ["A1", "A2"]) {
      await w.take(live(id), true);
      w.takeChat({ id: ALI, unreadCount: 1 }, true); // Baileys' chats.update after each message
    }
    expect(wa.store.chat(ALI)?.unread).toBe(2);
    w.takeChat({ id: ALI, unreadCount: 0 }, true); // read on the phone
    expect(wa.store.chat(ALI)?.unread).toBe(0);
    w.takeChat({ id: ALI, unreadCount: -1 }, true); // marked unread on the phone
    expect(wa.store.chat(ALI)?.unread).toBe(1);
    w.takeChat({ id: ALI, unreadCount: 5 }); // the history: the whole count
    expect(wa.store.chat(ALI)?.unread).toBe(5);
  });

  it("shows a mention by the person's name, by number or private id, and leaves unknown ones", () => {
    const wa = open();
    wa.store.upsertContact({ jid: ALI, name: "Ali Morgan" });
    wa.store.mapLid("99999999@lid", ALI);
    expect(wa.named("hi @99999999, @15550000002 and @12345678")).toBe("hi @Ali Morgan, @Ali Morgan and @12345678");
    expect(wa.named("mail me at a@b.c")).toBe("mail me at a@b.c");
  });
});
