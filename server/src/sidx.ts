// Finds the init segment (ftyp+moov) and the sidx index of a fragmented MP4 by walking
// box headers. Only headers are needed: start + size of each box give both ranges.
// Results never change for a video+itag, so they are cached without expiry.

export interface SegmentRanges {
  initRange: string; // "0-N"
  indexRange: string; // "A-B"
}

// Returns `want` bytes starting at `start` (or fewer at end of file).
export type ByteFetcher = (start: number, end: number) => Promise<Uint8Array>;

const CHUNK = 65536;
const MAX_BOXES = 32;

class Window {
  private start = 0;
  private bytes: Uint8Array = new Uint8Array(0);

  constructor(private readonly fetchBytes: ByteFetcher) {}

  // Makes [offset, offset+len) available; returns false at end of file.
  async ensure(offset: number, len: number): Promise<boolean> {
    if (offset >= this.start && offset + len <= this.start + this.bytes.length) return true;
    const end = Math.max(offset + len, offset + CHUNK) - 1;
    this.bytes = await this.fetchBytes(offset, end);
    this.start = offset;
    return this.bytes.length >= len;
  }

  view(offset: number, len: number): DataView {
    return new DataView(this.bytes.buffer, this.bytes.byteOffset + offset - this.start, len);
  }
}

export async function probeRanges(fetchBytes: ByteFetcher): Promise<SegmentRanges> {
  const win = new Window(fetchBytes);
  let offset = 0;
  let moovEnd: number | undefined;
  for (let i = 0; i < MAX_BOXES; i++) {
    if (!(await win.ensure(offset, 8))) break;
    const head = win.view(offset, 8);
    let size = head.getUint32(0);
    const type = String.fromCharCode(head.getUint8(4), head.getUint8(5), head.getUint8(6), head.getUint8(7));
    if (size === 1) {
      if (!(await win.ensure(offset, 16))) break;
      size = Number(win.view(offset + 8, 8).getBigUint64(0));
    } else if (size === 0) {
      break; // box runs to end of file: nothing after it
    }
    if (size < 8) throw new Error(`corrupt box ${JSON.stringify(type)} at ${offset}`);
    if (type === "moov") moovEnd = offset + size;
    if (type === "sidx") {
      if (moovEnd === undefined) throw new Error("sidx before moov");
      return { initRange: `0-${moovEnd - 1}`, indexRange: `${offset}-${offset + size - 1}` };
    }
    if (type === "moof" || type === "mdat") break; // media started without an index
    offset += size;
  }
  throw new Error("no sidx box found");
}

export class SidxCache {
  private readonly ranges = new Map<string, Promise<SegmentRanges>>();

  get(key: string, fetchBytes: ByteFetcher): Promise<SegmentRanges> {
    let hit = this.ranges.get(key);
    if (!hit) {
      hit = probeRanges(fetchBytes);
      hit.catch(() => this.ranges.delete(key)); // retry failures next time
      this.ranges.set(key, hit);
      if (this.ranges.size > 5000) this.ranges.delete(this.ranges.keys().next().value as string);
    }
    return hit;
  }
}
