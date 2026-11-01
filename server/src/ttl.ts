// Small in-memory TTL cache for browse responses. It stores the promise, so concurrent opens of
// the same screen share one youtubei.js request; a rejected load is dropped instead of cached.

export class TtlCache<V> {
  private readonly entries = new Map<string, { expires: number; value: Promise<V> }>();

  // `ttl` is fixed (ms from the load starting) or chosen per value once the load resolves, so lyrics
  // keep hits longer than misses. A per-value entry does not expire while its load is pending, so
  // those loads must be time-bounded.
  constructor(
    private readonly ttl: number | ((value: V) => number),
    private readonly maxEntries = 200,
    private readonly now: () => number = Date.now,
  ) {}

  get(key: string, load: () => Promise<V>): Promise<V> {
    const hit = this.entries.get(key);
    if (hit && hit.expires > this.now()) return hit.value;
    const value = load();
    const ttl = this.ttl;
    const entry = { expires: typeof ttl === "number" ? this.now() + ttl : Infinity, value };
    this.entries.delete(key); // re-insert so Map order stays oldest-first
    this.entries.set(key, entry);
    if (typeof ttl === "function") {
      value.then(
        (v) => {
          entry.expires = this.now() + ttl(v);
        },
        () => {},
      );
    }
    value.catch(() => {
      if (this.entries.get(key)?.value === value) this.entries.delete(key);
    });
    while (this.entries.size > this.maxEntries) {
      const oldest = this.entries.keys().next().value;
      if (oldest === undefined) break;
      this.entries.delete(oldest);
    }
    return value;
  }

  clear(): void {
    this.entries.clear();
  }
}
