// Android 4.2's time zone files (/system/usr/share/zoneinfo: zoneinfo.idx, zoneinfo.dat,
// zoneinfo.version), from IANA tzdata compiled by zic. The firmware ships 2012h, whose rules are long
// out of date (Russia, Turkey, Mexico and others changed since); bionic reads only these files.
//
//   zic -b fat -d <dir> africa antarctica asia australasia europe northamerica southamerica etcetera backward
//   node tools/firmware/zoneinfo.mjs <dir> <version, e.g. 2026f> <out dir>
//
// zoneinfo.dat is the zones' TZif files one after another. zoneinfo.idx has a 52-byte entry for each,
// sorted by name: the name (40 bytes, zero-padded), then big-endian int32s: where its TZif starts in
// the .dat, its length, and its standard offset now in milliseconds (what libcore's ZoneCompactor
// wrote; bionic reads only the first two). -b fat matters: bionic 4.2 (32-bit time_t) reads the TZif's
// version-1 data, which "slim" files leave empty.
import { readFileSync, readdirSync, statSync, writeFileSync, mkdirSync } from "node:fs";
import { join, relative } from "node:path";

const [dir, version, out] = process.argv.slice(2);
if (!dir || !/^\d{4}[a-z]$/.test(version ?? "") || !out) {
  console.error("usage: node zoneinfo.mjs <zic output dir> <version, e.g. 2026f> <out dir>");
  process.exit(2);
}

const files = [];
const walk = (d) => {
  for (const name of readdirSync(d)) {
    const p = join(d, name);
    if (statSync(p).isDirectory()) walk(p);
    else files.push(relative(dir, p));
  }
};
walk(dir);

// the standard offset (seconds) in effect now: the latest transition's type, or the last standard
// type before it while summer time is on; with no transitions, type 0
function rawOffset(tzif) {
  if (tzif.toString("latin1", 0, 4) !== "TZif") throw new Error("not TZif");
  const h = (i) => tzif.readInt32BE(20 + 4 * i);
  const [isutcnt, isstdcnt, leapcnt, timecnt, typecnt] = [h(0), h(1), h(2), h(3), h(4)];
  const times = 44, idx = times + 4 * timecnt, types = idx + timecnt;
  const type = (k) => ({ off: tzif.readInt32BE(types + 6 * k), dst: tzif[types + 6 * k + 4] });
  const now = Math.floor(Date.now() / 1000);
  let at = -1;
  for (let i = 0; i < timecnt; i++) if (tzif.readInt32BE(times + 4 * i) <= now) at = i;
  if (at < 0) return type(0).off;
  for (let i = at; i >= 0; i--) {
    const t = type(tzif[idx + i]);
    if (!t.dst) return t.off;
  }
  void isutcnt, void isstdcnt, void leapcnt, void typecnt;
  return type(tzif[idx + at]).off;
}

const zones = files
  .filter((f) => !["posixrules", "localtime", "Factory"].includes(f))
  .map((name) => ({ name, data: readFileSync(join(dir, name)) }))
  .filter((z) => z.data.toString("latin1", 0, 4) === "TZif" && Buffer.byteLength(z.name) < 40)
  .sort((a, b) => (a.name < b.name ? -1 : a.name > b.name ? 1 : 0));

const idx = Buffer.alloc(52 * zones.length);
const parts = [];
let at = 0;
zones.forEach((z, i) => {
  idx.write(z.name, 52 * i, "latin1");
  idx.writeInt32BE(at, 52 * i + 40);
  idx.writeInt32BE(z.data.length, 52 * i + 44);
  idx.writeInt32BE(rawOffset(z.data) * 1000, 52 * i + 48);
  parts.push(z.data);
  at += z.data.length;
});
mkdirSync(out, { recursive: true });
writeFileSync(join(out, "zoneinfo.idx"), idx);
writeFileSync(join(out, "zoneinfo.dat"), Buffer.concat(parts));
writeFileSync(join(out, "zoneinfo.version"), `${version}\n`);
console.log(`${zones.length} zones, tzdata ${version}: zoneinfo.idx ${idx.length} B, zoneinfo.dat ${at} B`);
