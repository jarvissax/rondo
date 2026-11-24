// What the Rondo shows of a WhatsApp message (whatsapp.ts keeps them): Baileys' WAMessage reduced to
// its kind, its words, its media's facts, what it quotes, and who sent it. Pure functions, no socket:
// the store and the tests both use them. A reaction, an edit and a deletion aren't messages of their
// own: they change one already kept (Change).

import { getContentType, normalizeMessageContent, type proto, type WAMessage } from "baileys";

import { say } from "./lang.js";

export type WaKind =
  | "text"
  | "image"
  | "video"
  | "voice"
  | "audio"
  | "document"
  | "sticker"
  | "location"
  | "contact"
  | "poll"
  | "deleted"
  | "unsupported";

export interface WaMedia {
  mime: string;
  seconds?: number;
  width?: number;
  height?: number;
  size?: number;
  fileName?: string;
  gif?: boolean;
}

export interface WaQuoted {
  id: string;
  sender: string;
  kind: WaKind;
  text: string; // raw: the quoted message's own words (its preview is made when it's sent, in the Rondo's language); before 3.6.1, its preview in English
  raw?: true;
  seconds?: number;
  fileName?: string;
  gif?: boolean;
}

export interface WaMessage {
  chat: string;
  id: string;
  fromMe: boolean;
  sender: string; // a person's jid ("" for me)
  senderName: string; // what they call themselves (pushName), for a group's names
  at: number; // ms
  kind: WaKind;
  text: string; // the words, or a media's caption
  media?: WaMedia;
  thumb?: string; // a media's tiny preview as WhatsApp sends it (JPEG, base64)
  quoted?: WaQuoted;
  status: number; // mine: 1 waiting, 2 sent, 3 delivered, 4 read, 5 played; theirs: 0
}

export type Change =
  | { kind: "react"; chat: string; target: string; sender: string; emoji: string }
  | { kind: "edit"; chat: string; target: string; text: string }
  | { kind: "delete"; chat: string; target: string };

// a person's or a group's jid, as the store keys its chats: the phone number's when WhatsApp gives it
// (a LID only when it doesn't), never a device's (":12")
export type JidResolver = (jid: string | null | undefined, alt?: string | null) => string;

const seconds = (v: unknown): number => {
  if (v == null) return 0;
  if (typeof v === "number") return v;
  if (typeof v === "object" && v !== null && "toNumber" in v && typeof (v as { toNumber: unknown }).toNumber === "function") {
    return (v as { toNumber: () => number }).toNumber();
  }
  return Number(v) || 0;
};

const num = (v: unknown): number | undefined => {
  const n = seconds(v);
  return n > 0 ? n : undefined;
};

const b64 = (v: Uint8Array | string | null | undefined): string | undefined => {
  if (!v) return undefined;
  if (typeof v === "string") return v || undefined;
  return v.length ? Buffer.from(v).toString("base64") : undefined;
};

// the words a message shows in a list or a quote ("Photo", "Voice message 0:12"), in the language of the
// Rondo asking (say(): the device's own words for them, lang/<code>.json)
export function previewText(kind: WaKind, text: string, media?: Pick<WaMedia, "seconds" | "gif" | "fileName">): string {
  const t = text.trim();
  const length = media?.seconds ? `${Math.floor(media.seconds / 60)}:${String(Math.floor(media.seconds % 60)).padStart(2, "0")}` : "";
  const captioned = (what: string) => (t ? say("whatsapp.summary.captioned", { what, text: t }) : what);
  switch (kind) {
    case "image":
      return captioned(say("whatsapp.photo"));
    case "video":
      return t ? captioned(say("whatsapp.video")) : media?.gif ? "GIF" : length ? say("whatsapp.videoLength", { length }) : say("whatsapp.video");
    case "voice":
      return length ? say("whatsapp.voiceMessageLength", { length }) : say("whatsapp.voiceMessage");
    case "audio":
      return length ? say("whatsapp.audioLength", { length }) : say("whatsapp.audio");
    case "document":
      return say("whatsapp.summary.document", { name: media?.fileName || t || say("whatsapp.file") });
    case "sticker":
      return say("whatsapp.sticker");
    case "deleted":
      return say("whatsapp.deleted");
    case "unsupported":
      return t || say("whatsapp.summary.unsupported");
    case "contact":
    case "poll":
      return nameless(kind, text);
    default:
      return t;
  }
}

// a contact or a poll that came without a name or a question ("👤 ", "📊 \n• Yes"): what they are, in
// the Rondo's language, filled in when it's shown (stored before 3.6.1: "Contact", "Poll" in English)
export function nameless(kind: WaKind, text: string): string {
  if (kind === "contact" && text.trim() === "\u{1F464}") return `\u{1F464} ${say("whatsapp.contact")}`;
  if (kind === "poll" && /^\u{1F4CA} ?(\n|$)/u.test(text)) return text.replace(/^\u{1F4CA} ?/u, `\u{1F4CA} ${say("whatsapp.poll")}`);
  return text;
}

interface Content {
  kind: WaKind;
  text: string;
  media?: WaMedia;
  thumb?: string;
  context?: proto.IContextInfo | null;
}

// the content of a message: its kind and the parts the Rondo shows
export function contentOf(message: proto.IMessage | null | undefined): Content | undefined {
  const m = normalizeMessageContent(message);
  if (!m) return undefined;
  const type = getContentType(m);
  switch (type) {
    case "conversation":
      return { kind: "text", text: m.conversation ?? "" };
    case "extendedTextMessage": {
      const x = m.extendedTextMessage!;
      return { kind: "text", text: x.text ?? "", context: x.contextInfo };
    }
    case "imageMessage": {
      const x = m.imageMessage!;
      return {
        kind: "image",
        text: x.caption ?? "",
        media: { mime: x.mimetype ?? "image/jpeg", width: num(x.width), height: num(x.height), size: num(x.fileLength) },
        thumb: b64(x.jpegThumbnail),
        context: x.contextInfo,
      };
    }
    case "videoMessage": {
      const x = m.videoMessage!;
      return {
        kind: "video",
        text: x.caption ?? "",
        media: {
          mime: x.mimetype ?? "video/mp4",
          seconds: num(x.seconds),
          width: num(x.width),
          height: num(x.height),
          size: num(x.fileLength),
          gif: !!x.gifPlayback || undefined,
        },
        thumb: b64(x.jpegThumbnail),
        context: x.contextInfo,
      };
    }
    case "audioMessage": {
      const x = m.audioMessage!;
      return {
        kind: x.ptt ? "voice" : "audio",
        text: "",
        media: { mime: x.mimetype ?? "audio/ogg; codecs=opus", seconds: num(x.seconds), size: num(x.fileLength) },
        context: x.contextInfo,
      };
    }
    case "documentMessage": {
      const x = m.documentMessage!;
      return {
        kind: "document",
        text: x.caption ?? "",
        media: { mime: x.mimetype ?? "application/octet-stream", size: num(x.fileLength), fileName: x.fileName ?? x.title ?? undefined },
        thumb: b64(x.jpegThumbnail),
        context: x.contextInfo,
      };
    }
    case "stickerMessage": {
      const x = m.stickerMessage!;
      return {
        kind: "sticker",
        text: "",
        media: { mime: x.mimetype ?? "image/webp", width: num(x.width), height: num(x.height), size: num(x.fileLength) },
        context: x.contextInfo,
      };
    }
    case "locationMessage":
    case "liveLocationMessage": {
      const x = (m.locationMessage ?? m.liveLocationMessage)!;
      const where = [("name" in x ? x.name : "") || "", ("address" in x ? x.address : "") || ""].filter(Boolean).join(", ");
      const lat = Number(x.degreesLatitude ?? 0).toFixed(5), lng = Number(x.degreesLongitude ?? 0).toFixed(5);
      return { kind: "location", text: `\u{1F4CD} ${where ? `${where} (${lat}, ${lng})` : `${lat}, ${lng}`}`, context: x.contextInfo };
    }
    case "contactMessage":
      return { kind: "contact", text: `\u{1F464} ${m.contactMessage!.displayName ?? ""}`, context: m.contactMessage!.contextInfo };
    case "contactsArrayMessage": {
      const x = m.contactsArrayMessage!;
      const names = (x.contacts ?? []).map((c) => c.displayName).filter(Boolean);
      return { kind: "contact", text: `\u{1F464} ${names.join(", ") || x.displayName || ""}`, context: x.contextInfo };
    }
    case "pollCreationMessage":
    case "pollCreationMessageV2":
    case "pollCreationMessageV3": {
      const x = (m.pollCreationMessage ?? m.pollCreationMessageV2 ?? m.pollCreationMessageV3)!;
      const options = (x.options ?? []).map((o) => `• ${o.optionName ?? ""}`).join("\n");
      return { kind: "poll", text: `\u{1F4CA} ${x.name ?? ""}${options ? `\n${options}` : ""}`, context: x.contextInfo };
    }
    default:
      return undefined;
  }
}

// a reaction, an edit or a deletion: what it changes
export function changeOf(msg: WAMessage, chat: string, sender: string): Change | undefined {
  const m = normalizeMessageContent(msg.message);
  if (!m) return undefined;
  if (m.reactionMessage?.key?.id) {
    return { kind: "react", chat, target: m.reactionMessage.key.id, sender, emoji: m.reactionMessage.text ?? "" };
  }
  const p = m.protocolMessage;
  if (p?.key?.id) {
    // REVOKE (0) and MESSAGE_EDIT (14) in WAProto's ProtocolMessage.Type
    if (p.type === 0) return { kind: "delete", chat, target: p.key.id };
    if (p.type === 14) {
      const edited = contentOf(p.editedMessage);
      if (edited) return { kind: "edit", chat, target: p.key.id, text: edited.text };
    }
  }
  return undefined;
}

// who wrote a group's message: the key's participant (live messages) or the message's own (history's,
// often a private id: a LID)
export function groupSender(msg: WAMessage, resolve: JidResolver): string {
  const key = msg.key as { participant?: string | null; participantAlt?: string | null } | undefined;
  return resolve(key?.participant || msg.participant, key?.participantAlt);
}

// a message as the Rondo shows it; undefined for what isn't one (a reaction, a protocol message, a
// status update, a group's system note)
export function reduceMessage(msg: WAMessage, resolve: JidResolver, myJid: string): WaMessage | undefined {
  const key = msg.key;
  if (!key?.id || !key.remoteJid) return undefined;
  const chat = resolve(key.remoteJid, (key as { remoteJidAlt?: string }).remoteJidAlt);
  if (!chat || chat === "status@broadcast" || chat.endsWith("@newsletter") || chat.endsWith("@broadcast")) return undefined;
  const fromMe = !!key.fromMe;
  const group = chat.endsWith("@g.us");
  const sender = fromMe ? myJid : group ? groupSender(msg, resolve) : chat;
  const at = seconds(msg.messageTimestamp) * 1000;
  const status = fromMe ? Math.max(1, Number(msg.status ?? 2)) : 0;
  if (msg.messageStubType && !msg.message) {
    // REVOKE as a stub (a message deleted before it came to this device)
    if (msg.messageStubType === 1) return { chat, id: key.id, fromMe, sender, senderName: msg.pushName ?? "", at, kind: "deleted", text: "", status };
    return undefined;
  }
  const c = contentOf(msg.message);
  if (!c) {
    if (changeOf(msg, chat, sender)) return undefined;
    const m = normalizeMessageContent(msg.message);
    const type = m ? getContentType(m) : undefined;
    // the inner workings of WhatsApp (keys, history notes, presence): not shown
    if (!type || /^(protocolMessage|senderKeyDistributionMessage|messageContextInfo|reactionMessage|pollUpdateMessage|keepInChatMessage|encReactionMessage|placeholderMessage)$/.test(type)) return undefined;
    return { chat, id: key.id, fromMe, sender, senderName: msg.pushName ?? "", at, kind: "unsupported", text: "", status };
  }
  const out: WaMessage = { chat, id: key.id, fromMe, sender, senderName: msg.pushName ?? "", at, kind: c.kind, text: c.text, status };
  if (c.media) out.media = c.media;
  if (c.thumb) out.thumb = c.thumb;
  const ctx = c.context;
  if (ctx?.stanzaId && ctx.quotedMessage) {
    const q = contentOf(ctx.quotedMessage);
    out.quoted = {
      id: ctx.stanzaId,
      sender: ctx.participant ? resolve(ctx.participant) : "",
      kind: q?.kind ?? "unsupported",
      // its own words: its preview is made when it's shown, in the language of the Rondo asking
      text: q?.text ?? "",
      raw: true,
      ...(q?.media?.seconds ? { seconds: q.media.seconds } : {}),
      ...(q?.media?.fileName ? { fileName: q.media.fileName } : {}),
      ...(q?.media?.gif ? { gif: true } : {}),
    };
  }
  return out;
}
