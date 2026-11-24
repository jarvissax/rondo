// WhatsApp's routes for the Rondo (device token; docs/dev/server-api.md): its state, the chats, a
// chat's messages, contacts with their profile, media and pictures, sending, and a feed of what changed
// that waits for news while the WhatsApp screen is open. Linking is the admin page's (admin.ts).

import { createReadStream } from "node:fs";
import { stat } from "node:fs/promises";
import type { FastifyInstance, FastifyReply } from "fastify";
import { say } from "./lang.js";
import type { WhatsApp } from "./whatsapp.js";

const JID = /^[0-9A-Za-z._:-]{1,64}@(s\.whatsapp\.net|g\.us|lid)$/;
const ID = /^[0-9A-Za-z_-]{1,64}$/;
const BODY_LIMIT = 64 * 1024 * 1024; // a video from the card

const bad = (reply: FastifyReply, what: string) => reply.code(400).send({ error: what });

export function registerWhatsAppRoutes(app: FastifyInstance, wa: WhatsApp): void {
  // the Y1's recordings and photos and videos come as the bytes themselves
  for (const type of ["audio/wav", "audio/x-wav", "image/jpeg", "image/png", "video/mp4", "application/octet-stream"]) {
    if (!app.hasContentTypeParser(type)) app.addContentTypeParser(type, { parseAs: "buffer", bodyLimit: BODY_LIMIT }, (_req, body, done) => done(null, body));
  }

  app.get("/wa/status", async () => wa.status());

  // linking and unlinking are the admin page's (admin.ts): the Rondo reads and sends

  // the chats, the newest first (archived ones with ?archived=1)
  app.get<{ Querystring: { archived?: string } }>("/wa/chats", async (req) => {
    const archived = req.query.archived === "1";
    const chats = wa.store.chats().filter((c) => c.archived === archived && (c.at > 0 || c.unread > 0));
    return { seq: wa.store.latest(), chats: chats.map((c) => wa.chatJson(c)) };
  });

  // a chat's messages before a time (ms), oldest first
  app.get<{ Params: { jid: string }; Querystring: { before?: string; limit?: string } }>("/wa/chats/:jid/messages", async (req, reply) => {
    const jid = req.params.jid;
    if (!JID.test(jid)) return bad(reply, "jid");
    const limit = Math.min(200, Math.max(1, Number(req.query.limit ?? 50) || 50));
    const before = Number(req.query.before ?? 0) || 0;
    const chat = wa.store.chat(jid);
    return {
      seq: wa.store.latest(),
      chat: chat ? wa.chatJson(chat) : { jid, name: wa.nameOf(jid), group: jid.endsWith("@g.us") },
      messages: wa.store.messages(jid, before, limit).map((m) => wa.messageJson(m)),
    };
  });

  app.post<{ Params: { jid: string } }>("/wa/chats/:jid/read", async (req, reply) => {
    if (!JID.test(req.params.jid)) return bad(reply, "jid");
    await wa.markRead(req.params.jid);
    return { ok: true };
  });

  const sent = async (reply: FastifyReply, work: () => Promise<object | undefined>) => {
    try {
      const m = await work();
      return m ?? reply.code(502).send({ error: say("whatsapp.server.notTaken") });
    } catch (err) {
      const why = (err as Error).message;
      if (why === "offline") return reply.code(503).send({ error: say("whatsapp.server.offline") });
      reply.log.warn({ err: why }, "whatsapp: not sent");
      return reply.code(502).send({ error: say("whatsapp.server.notSent") });
    }
  };

  app.post<{ Params: { jid: string }; Body: { text?: string; quoted?: string } }>("/wa/chats/:jid/text", async (req, reply) => {
    const jid = req.params.jid, text = String(req.body?.text ?? "").trim();
    if (!JID.test(jid)) return bad(reply, "jid");
    if (!text || text.length > 4000) return bad(reply, "text");
    const quoted = req.body?.quoted && ID.test(req.body.quoted) ? req.body.quoted : undefined;
    return sent(reply, async () => {
      const m = await wa.sendText(jid, text, quoted);
      return m ? wa.messageJson(m) : undefined;
    });
  });

  // a reaction from the Rondo (server 3.4.0): any emoji, "" to take it back
  app.post<{ Params: { jid: string }; Body: { id?: string; emoji?: string } }>("/wa/chats/:jid/react", async (req, reply) => {
    const jid = req.params.jid, id = String(req.body?.id ?? ""), emoji = String(req.body?.emoji ?? "");
    if (!JID.test(jid) || !ID.test(id)) return bad(reply, "id");
    if ([...emoji].length > 12) return bad(reply, "emoji");
    return sent(reply, async () => {
      const m = await wa.react(jid, id, emoji);
      return m ? wa.messageJson(m) : undefined;
    });
  });

  app.post<{ Params: { jid: string } }>("/wa/chats/:jid/voice", { bodyLimit: BODY_LIMIT }, async (req, reply) => {
    const jid = req.params.jid;
    if (!JID.test(jid)) return bad(reply, "jid");
    const body = req.body;
    if (!Buffer.isBuffer(body) || body.length < 100) return bad(reply, "audio");
    return sent(reply, async () => {
      const m = await wa.sendVoice(jid, body);
      return m ? wa.messageJson(m) : undefined;
    });
  });

  app.post<{ Params: { jid: string }; Querystring: { kind?: string; caption?: string } }>("/wa/chats/:jid/media", { bodyLimit: BODY_LIMIT }, async (req, reply) => {
    const jid = req.params.jid, kind = req.query.kind;
    if (!JID.test(jid)) return bad(reply, "jid");
    if (kind !== "image" && kind !== "video") return bad(reply, "kind");
    const body = req.body;
    if (!Buffer.isBuffer(body) || body.length < 100) return bad(reply, "file");
    return sent(reply, async () => {
      const m = await wa.sendMedia(jid, kind, body, String(req.query.caption ?? "").slice(0, 1000));
      return m ? wa.messageJson(m) : undefined;
    });
  });

  // a message's media: a photo or a sticker as a picture w wide (480 by default), a video as the Y1
  // plays it, a voice message or a document as it is; ?preview=1: the small picture WhatsApp sends
  // with a photo or a video (what a chat shows before it's opened), w wide
  app.get<{ Params: { jid: string; id: string }; Querystring: { w?: string; preview?: string } }>("/wa/media/:jid/:id", async (req, reply) => {
    const { jid, id } = req.params;
    if (!JID.test(jid) || !ID.test(id)) return bad(reply, "id");
    const m = wa.store.message(jid, id);
    if (!m?.media) return reply.code(404).send({ error: "none" });
    const w = Math.min(1280, Math.max(32, Number(req.query.w ?? 480) || 480));
    if (req.query.preview === "1" && m.thumb) {
      const pic = await wa.pictureOf(Buffer.from(m.thumb, "base64"), w).catch(() => undefined);
      if (pic) return reply.type("image/jpeg").header("Cache-Control", "private, max-age=604800").send(pic);
    }
    if (req.query.preview === "1" && m.kind !== "image" && m.kind !== "sticker") return reply.code(404).send({ error: "none" });
    try {
      if (m.kind === "image" || m.kind === "sticker") {
        const pic = await wa.picture(await wa.mediaFile(jid, id), w, m.kind === "sticker");
        return reply.type(m.kind === "sticker" ? "image/png" : "image/jpeg").header("Cache-Control", "private, max-age=604800").send(pic);
      }
      const file = m.kind === "video" ? await wa.videoForY1(jid, id) : await wa.mediaFile(jid, id);
      const size = (await stat(file)).size;
      const type = m.kind === "video" ? "video/mp4" : m.media.mime.split(";")[0]!;
      reply.header("Content-Length", size).header("Accept-Ranges", "none").type(type);
      return reply.send(createReadStream(file));
    } catch (err) {
      req.log.warn({ err: (err as Error).message }, "whatsapp: media unavailable");
      return reply.code(410).send({ error: say("whatsapp.server.gone") });
    }
  });

  // a person's or a group's picture (s or w wide, 96 by default: the Y1 asks as it asks for thumbnails);
  // 404 when they have none
  app.get<{ Params: { jid: string }; Querystring: { s?: string; w?: string } }>("/wa/avatar/:jid", async (req, reply) => {
    const jid = req.params.jid;
    if (!JID.test(jid)) return bad(reply, "jid");
    const file = await wa.avatarFile(jid);
    if (!file) return reply.code(404).send({ error: "none" });
    const s = Math.min(640, Math.max(24, Number(req.query.s ?? req.query.w ?? 96) || 96));
    return reply.type("image/jpeg").header("Cache-Control", "private, max-age=86400").send(await wa.picture(file, s));
  });

  // everyone: the contacts saved on the phone first (by name), then those known by their own name
  app.get("/wa/contacts", async () => {
    const all = wa.store
      .contacts()
      .filter((c) => c.jid.endsWith("@s.whatsapp.net") && (c.name || c.notify))
      .map((c) => wa.contactJson(c.jid) as { name: string; saved: boolean });
    // as the phone lists them: names that start with a letter first, then the rest ("+1 555…", "@name"), A to Z
    const letter = (s: string) => Number(/^\p{L}/u.test(s.replace(/^~/, "")));
    all.sort((a, b) => Number(b.saved) - Number(a.saved) || letter(b.name) - letter(a.name) || a.name.localeCompare(b.name));
    return { contacts: all };
  });

  // one person's (or a group's) profile: name, number, about
  app.get<{ Params: { jid: string } }>("/wa/contacts/:jid", async (req, reply) => {
    const jid = req.params.jid;
    if (!JID.test(jid)) return bad(reply, "jid");
    const about = jid.endsWith("@g.us") ? "" : await wa.about(jid);
    return wa.contactJson(jid, about);
  });

  // what changed after `since` (chats and messages), waiting up to `wait` s (at most 25) for some
  app.get<{ Querystring: { since?: string; wait?: string } }>("/wa/updates", async (req) => {
    const since = Number(req.query.since ?? 0) || 0;
    const wait = Math.min(25, Math.max(0, Number(req.query.wait ?? 0) || 0));
    if (wait) await wa.waitFor(since, wait * 1000);
    return {
      seq: wa.store.latest(),
      status: wa.status().state,
      chats: wa.store.chats(since).map((c) => wa.chatJson(c)),
      messages: wa.store.messagesSince(since).map((m) => ({ chat: m.chat, ...wa.messageJson(m) })),
    };
  });
}
