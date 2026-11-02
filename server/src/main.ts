import { buildApp } from "./app.js";
import { loadConfig } from "./config.js";
import { githubJson, Updates } from "./updates.js";

const config = loadConfig();
const { app, health, admin, tv } = await buildApp(config, { updates: new Updates(config.updateCheck ? githubJson : null) });

health.start();
admin.start();
admin.announce(); // the setup code, on a fresh server
void tv.load(); // the saved code sign-in, if any

const shutdown = async (signal: string) => {
  app.log.info({ signal }, "shutting down");
  health.stop();
  admin.stop();
  await app.close();
  process.exit(0);
};
process.on("SIGTERM", () => void shutdown("SIGTERM"));
process.on("SIGINT", () => void shutdown("SIGINT"));

await app.listen({ host: "0.0.0.0", port: config.port });
