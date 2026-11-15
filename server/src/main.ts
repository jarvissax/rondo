import { buildApp } from "./app.js";
import { loadConfig } from "./config.js";
import { ServerUpdate } from "./serverUpdate.js";
import { githubJson, Updates } from "./updates.js";
import { WhatsApp } from "./whatsapp.js";

const config = loadConfig();
const { app, health, admin, tv, whatsapp } = await buildApp(config, {
  updates: new Updates(config.updateCheck ? githubJson : null),
  serverUpdate: new ServerUpdate(config.updaterUrl, config.updaterTokenFile),
  whatsapp: (log) => new WhatsApp(config.whatsappDir, log),
});

health.start();
admin.start();
admin.announce(); // the setup code, on a fresh server
void tv.load(); // the saved code sign-in, if any
void whatsapp?.start().catch((err: Error) => app.log.warn({ err: err.message }, "whatsapp: didn't start")); // linked: connects

const shutdown = async (signal: string) => {
  app.log.info({ signal }, "shutting down");
  health.stop();
  admin.stop();
  whatsapp?.stop();
  await app.close();
  process.exit(0);
};
process.on("SIGTERM", () => void shutdown("SIGTERM"));
process.on("SIGINT", () => void shutdown("SIGINT"));

await app.listen({ host: "0.0.0.0", port: config.port });
