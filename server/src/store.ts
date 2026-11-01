// What the admin page changes, kept on the data volume (/data/config.json): the admin password
// (scrypt hash) and the one device's token (SHA-256). Secrets are stored only as hashes and never
// returned by any endpoint (docs/dev/server-api.md).

import { createHash, randomBytes, scryptSync, timingSafeEqual } from "node:crypto";
import { mkdirSync, readFileSync, renameSync, writeFileSync } from "node:fs";
import { dirname } from "node:path";

export interface StoreData {
  adminHash?: string;
  device?: { tokenHash: string; pairedAt: number; name?: string };
  // set by the first pairing or disconnect: API_TOKEN from the environment no longer works
  envRetired?: boolean;
}

const SCRYPT = { N: 16384, r: 8, p: 1, keylen: 32 };

export function hashPassword(password: string): string {
  const salt = randomBytes(16);
  const hash = scryptSync(password, salt, SCRYPT.keylen, { N: SCRYPT.N, r: SCRYPT.r, p: SCRYPT.p });
  return `scrypt$${SCRYPT.N}$${SCRYPT.r}$${SCRYPT.p}$${salt.toString("base64")}$${hash.toString("base64")}`;
}

export function verifyPassword(password: string, stored: string): boolean {
  const [kind, n, r, p, salt, hash] = stored.split("$");
  if (kind !== "scrypt" || !salt || !hash) return false;
  const expected = Buffer.from(hash, "base64");
  const actual = scryptSync(password, Buffer.from(salt, "base64"), expected.length, { N: Number(n), r: Number(r), p: Number(p) });
  return actual.length === expected.length && timingSafeEqual(actual, expected);
}

export function sha256(value: string): string {
  return createHash("sha256").update(value).digest("hex");
}

export class Store {
  private current: StoreData;

  constructor(private readonly file: string) {
    this.current = Store.read(file);
  }

  private static read(file: string): StoreData {
    try {
      return JSON.parse(readFileSync(file, "utf8")) as StoreData;
    } catch {
      return {};
    }
  }

  get data(): Readonly<StoreData> {
    return this.current;
  }

  // atomic write: a crash mid-write must not lose the admin password
  update(change: (d: StoreData) => StoreData): void {
    const next = change(structuredClone(this.current));
    mkdirSync(dirname(this.file), { recursive: true });
    const tmp = `${this.file}.tmp`;
    writeFileSync(tmp, JSON.stringify(next, null, 2), { mode: 0o600 });
    renameSync(tmp, this.file);
    this.current = next;
  }
}
