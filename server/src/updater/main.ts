// Rondo's updater: a small companion container (infra/compose.*.yaml) that moves the Rondo server's
// container onto the newest server image when the admin page asks. It needs the Docker socket, and
// only ever touches the one container named in TARGET. Its API is reachable only on the compose
// network, and every call needs the token it keeps in TOKEN_FILE, a small volume it shares with the
// server (read-only there). Built into its own image by infra/updater.Dockerfile.
//
//   GET  /v1/status  → {state, message, at, target}
//   POST /v1/update  → 202 {started: true}; 409 while an update runs

import { randomBytes, timingSafeEqual } from "node:crypto";
import { chmodSync, existsSync, mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { createServer, type IncomingMessage, type ServerResponse } from "node:http";
import { dirname } from "node:path";
import { Docker } from "./docker.js";
import { updateContainer } from "./recreate.js";

const TARGET = process.env.TARGET || "rondo";
const PORT = Number(process.env.PORT) || 8080;
const TOKEN_FILE = process.env.TOKEN_FILE || "/run/updater/token";
const HEALTH_URL = process.env.HEALTH_URL || `http://${TARGET}:3000/healthz`;
const docker = new Docker(process.env.DOCKER_SOCKET || "/var/run/docker.sock");

const log = (msg: string) => console.log(`${new Date().toISOString()} ${msg}`);

function loadToken(): string {
  if (existsSync(TOKEN_FILE)) {
    const t = readFileSync(TOKEN_FILE, "utf8").trim();
    if (t.length >= 32) return t;
  }
  mkdirSync(dirname(TOKEN_FILE), { recursive: true });
  const t = randomBytes(24).toString("hex");
  writeFileSync(TOKEN_FILE, t + "\n");
  chmodSync(TOKEN_FILE, 0o644); // the server reads it as its own user
  return t;
}
const token = Buffer.from(loadToken());

type State = "idle" | "pulling" | "restarting" | "updated" | "current" | "failed";
let status: { state: State; message: string; at: number } = { state: "idle", message: "", at: Date.now() };
const busy = () => status.state === "pulling" || status.state === "restarting";
const set = (state: State, message: string) => {
  status = { state, message, at: Date.now() };
  log(`${state}: ${message}`);
};

async function answers(): Promise<boolean> {
  const res = await fetch(HEALTH_URL, { signal: AbortSignal.timeout(5000) });
  return res.ok;
}

async function update() {
  try {
    const r = await updateContainer(docker, TARGET, { answers, progress: set });
    set(r.updated ? "updated" : "current", r.message);
  } catch (err) {
    set("failed", String((err as Error)?.message ?? err).slice(0, 300));
  }
}

function authorized(req: IncomingMessage): boolean {
  const given = Buffer.from(String(req.headers.authorization ?? "").replace(/^Bearer\s+/i, ""));
  return given.length === token.length && timingSafeEqual(given, token);
}

function send(res: ServerResponse, code: number, body: unknown) {
  res.writeHead(code, { "content-type": "application/json", "cache-control": "no-store" });
  res.end(JSON.stringify(body));
}

createServer((req, res) => {
  if (!authorized(req)) return send(res, 401, { error: "token" });
  const path = (req.url ?? "").split("?")[0];
  if (req.method === "GET" && path === "/v1/status") return send(res, 200, { ...status, target: TARGET });
  if (req.method === "POST" && path === "/v1/update") {
    if (busy()) return send(res, 409, { error: "busy", ...status });
    set("pulling", "Starting");
    void update();
    return send(res, 202, { started: true });
  }
  return send(res, 404, { error: "not_found" });
}).listen(PORT, () => log(`Rondo updater: watching ${TARGET}, on port ${PORT}`));
