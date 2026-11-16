// Builds emoji.ttf for a Y1 from the emoji font of your own Mac: its first face, with only the one bitmap
// size Rondo uses (48 px; macOS's font has nine, about 190 MB), the signature dropped. Apple's emoji are
// Apple's: this only converts the copy on your Mac, for your own Y1. The font is never part of Rondo,
// its repository or its releases (.gitignore keeps emoji.ttf out). docs/using.md has how to put it on the Y1.
//   node tools/emoji/make-font.cjs [font] [px] [out]
//   defaults: "/System/Library/Fonts/Apple Color Emoji.ttc", 48, ./emoji.ttf
const fs = require("fs");
const path = require("path");
const src = process.argv[2] || "/System/Library/Fonts/Apple Color Emoji.ttc";
const want = Number(process.argv[3] || 48);
const b = fs.readFileSync(src);
let off = 0;
if (b.toString("latin1", 0, 4) === "ttcf") off = b.readUInt32BE(12);
const sfnt = b.readUInt32BE(off);
const n = b.readUInt16BE(off + 4);
const tables = [];
for (let i = 0; i < n; i++) {
  const r = off + 12 + i * 16;
  tables.push({ tag: b.toString("latin1", r, r + 4), off: b.readUInt32BE(r + 8), len: b.readUInt32BE(r + 12) });
}
const get = (t) => tables.find((x) => x.tag === t);
const glyphs = b.readUInt16BE(get("maxp").off + 4);
// sbix: the one strike wanted, copied as it is (its glyph offsets count from the strike's start)
const sb = get("sbix"), s0 = sb.off, strikes = b.readUInt32BE(s0 + 4);
let strike = -1;
for (let k = 0; k < strikes; k++) if (b.readUInt16BE(s0 + b.readUInt32BE(s0 + 8 + 4 * k)) === want) strike = k;
if (strike < 0) throw new Error(`no ${want} px size in ${src}`);
const so = s0 + b.readUInt32BE(s0 + 8 + 4 * strike);
const end = b.readUInt32BE(so + 4 + 4 * glyphs);  // after the last glyph's data
const sbix = Buffer.alloc(12 + end);
b.copy(sbix, 0, s0, s0 + 4);  // version, flags
sbix.writeUInt32BE(1, 4);
sbix.writeUInt32BE(12, 8);
b.copy(sbix, 12, so, so + end);
// the font: every other table as it is (no DSIG: it would no longer match), checksums and head's adjustment
const out = tables.filter((t) => t.tag !== "DSIG").map((t) => ({ tag: t.tag, data: t.tag === "sbix" ? sbix : Buffer.from(b.subarray(t.off, t.off + t.len)) }));
out.sort((a, z) => (a.tag < z.tag ? -1 : 1));
const sum = (buf) => { let s = 0; const p = Buffer.concat([buf, Buffer.alloc((4 - (buf.length % 4)) % 4)]); for (let i = 0; i < p.length; i += 4) s = (s + p.readUInt32BE(i)) >>> 0; return s; };
const head = out.find((t) => t.tag === "head").data;
head.writeUInt32BE(0, 8);
const N = out.length, pow = 2 ** Math.floor(Math.log2(N));
const dir = Buffer.alloc(12 + 16 * N);
dir.writeUInt32BE(sfnt, 0);
dir.writeUInt16BE(N, 4);
dir.writeUInt16BE(pow * 16, 6);
dir.writeUInt16BE(Math.log2(pow), 8);
dir.writeUInt16BE(N * 16 - pow * 16, 10);
let at = dir.length;
const parts = [dir];
out.forEach((t, i) => {
  const r = 12 + i * 16;
  dir.write(t.tag, r, 4, "latin1");
  dir.writeUInt32BE(sum(t.data), r + 4);
  dir.writeUInt32BE(at, r + 8);
  dir.writeUInt32BE(t.data.length, r + 12);
  const pad = Buffer.alloc((4 - (t.data.length % 4)) % 4);
  parts.push(t.data, pad);
  at += t.data.length + pad.length;
});
const font = Buffer.concat(parts);
const ho = dir.readUInt32BE(12 + out.findIndex((t) => t.tag === "head") * 16 + 8);
font.writeUInt32BE((0xb1b0afba - sum(font)) >>> 0, ho + 8);
const dest = path.resolve(process.argv[4] || "emoji.ttf");
fs.writeFileSync(dest, font);
console.log(`${dest}: ${(font.length / 1048576).toFixed(1)} MB, ${N} tables, one ${want} px size, ${glyphs} glyphs`);
