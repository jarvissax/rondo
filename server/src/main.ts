import { buildApp } from "./app.js";
import { loadConfig } from "./config.js";

const config = loadConfig();
const { app, health, admin, tv } = await buildApp(config);

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
