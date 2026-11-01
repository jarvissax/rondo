// Synthetic fragmented-MP4 byte streams for tests: only box headers matter.

export function box(type: string, size: number, largesize = false): Uint8Array {
  const b = new Uint8Array(size);
  const v = new DataView(b.buffer);
  if (largesize) {
    v.setUint32(0, 1);
    v.setBigUint64(8, BigInt(size));
  } else {
    v.setUint32(0, size);
  }
  for (let i = 0; i < 4; i++) b[4 + i] = type.charCodeAt(i);
  return b;
}

export function concat(...parts: Uint8Array[]): Uint8Array {
  const out = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
  let o = 0;
  for (const p of parts) {
    out.set(p, o);
    o += p.length;
  }
  return out;
}

// ftyp(24) + moov(moovSize) + sidx(100) + moof(200) + mdat(mdatSize)
export function dashFile(moovSize = 2000, mdatSize = 5000): Uint8Array {
  return concat(box("ftyp", 24), box("moov", moovSize), box("sidx", 100), box("moof", 200), box("mdat", mdatSize));
}
