import { describe, expect, it } from "vitest";
import { type Chunk, Pager, TokenStore } from "../src/paging.js";

describe("TokenStore", () => {
  it("serves a token until its TTL, then forgets it", () => {
    let now = 0;
    const store = new TokenStore<string>(30 * 60_000, 500, () => now);
    const token = store.put("page2");
    expect(token).toMatch(/^[A-Za-z0-9_-]{22}$/);
    expect(store.get(token)).toBe("page2");
    expect(store.get(token)).toBe("page2"); // reusable, for retries
    now = 30 * 60_000 - 1;
    expect(store.get(token)).toBe("page2");
    now = 30 * 60_000;
    expect(store.get(token)).toBeUndefined();
    expect(store.size).toBe(0);
    expect(store.get("not-a-token")).toBeUndefined();
  });

  it("keeps at most maxEntries, dropping the oldest, and drops expired ones on put", () => {
    let now = 0;
    const store = new TokenStore<number>(1000, 3, () => now);
    const [a, b, c, d] = [1, 2, 3, 4].map((n) => store.put(n));
    expect(store.size).toBe(3);
    expect(store.get(a!)).toBeUndefined();
    expect([store.get(b!), store.get(c!), store.get(d!)]).toEqual([2, 3, 4]);
    now = 1500;
    const e = store.put(5);
    expect(store.size).toBe(1);
    expect(store.get(e)).toBe(5);
    store.clear();
    expect(store.get(e)).toBeUndefined();
  });

  it("makes unguessable, distinct tokens", () => {
    const store = new TokenStore<number>();
    const tokens = new Set(Array.from({ length: 200 }, (_, i) => store.put(i)));
    expect(tokens.size).toBe(200);
  });
});

// a fake YouTube list: pages of ids, each page continuing to the next
function source(pages: Array<Array<string | undefined>>, calls: { n: number } = { n: 0 }) {
  const chunk = (i: number): Chunk<{ id: string }> => ({
    items: pages[i]!.map((id) => (id ? { id } : undefined)),
    more:
      i + 1 < pages.length
        ? async () => {
            calls.n++;
            return chunk(i + 1);
          }
        : undefined,
  });
  return async () => chunk(0);
}
const byId = { key: (i: { id: string }) => i.id, min: 2 };
const ids = (p: { items: unknown[] }) => (p.items as Array<{ id: string }>).map((i) => i.id);

describe("Pager", () => {
  it("answers a page with a next token, and the last page without one", async () => {
    const pager = new Pager();
    const p1 = await pager.page("video/home", source([["a", "b"], ["c", "d"], ["e", "f"]]), byId);
    expect(ids(p1)).toEqual(["a", "b"]);
    expect(p1.next).toBeTypeOf("string");
    const p2 = await pager.resume("video/home", p1.next!)!();
    expect(ids(p2)).toEqual(["c", "d"]);
    const p3 = await pager.resume("video/home", p2.next!)!();
    expect(ids(p3)).toEqual(["e", "f"]);
    expect(p3).not.toHaveProperty("next");
  });

  it("does not resume a token from another list, or an unknown one", async () => {
    const pager = new Pager();
    const p1 = await pager.page("music/search", source([["a", "b"], ["c", "d"]]), byId);
    expect(pager.resume("video/search", p1.next!)).toBeUndefined();
    expect(pager.resume("music/search", "nope")).toBeUndefined();
    pager.clear();
    expect(pager.resume("music/search", p1.next!)).toBeUndefined();
  });

  it("expires tokens after 30 minutes", async () => {
    let now = 0;
    const pager = new Pager(new TokenStore(30 * 60_000, 500, () => now));
    const p1 = await pager.page("video/home", source([["a", "b"], ["c", "d"]]), byId);
    now = 30 * 60_000;
    expect(pager.resume("video/home", p1.next!)).toBeUndefined();
  });

  it("drops items already shown on an earlier page, and gives the same page on a retry", async () => {
    const pager = new Pager();
    const p1 = await pager.page("video/home", source([["a", "b"], ["b", "c", "a", "d"], ["d", "e"]]), byId);
    const again = pager.resume("video/home", p1.next!)!;
    const p2 = await again();
    expect(ids(p2)).toEqual(["c", "d"]);
    expect(ids(await again())).toEqual(["c", "d"]);
    expect(ids(await pager.resume("video/home", p2.next!)!())).toEqual(["e"]);
  });

  it("starts from a seed of ids to leave out (the video a related list is for)", async () => {
    const pager = new Pager();
    const p = await pager.page("video/related/x", source([["x", "a", "b"]]), byId, new Set(["x"]));
    expect(ids(p)).toEqual(["a", "b"]);
  });

  it("keeps fetching while a page reduces to too few items, up to maxHops", async () => {
    const calls = { n: 0 };
    const pager = new Pager();
    const p1 = await pager.page("video/home", source([[undefined, "a"], [undefined], ["b", "c"], ["d", "e"]], calls), byId);
    expect(ids(p1)).toEqual(["a", "b", "c"]);
    expect(calls.n).toBe(2);
    const capped = await pager.page("video/home", source([[], [], [], ["late"]]), { ...byId, maxHops: 1 });
    expect(capped.items).toEqual([]);
    expect(capped.next).toBeTypeOf("string"); // the app can still ask for more
  });

  it("keeps sections without a key, duplicates included", async () => {
    const pager = new Pager();
    const p = await pager.page("music/home", source([["a", "a"]]), { min: 1 });
    expect(ids(p)).toEqual(["a", "a"]);
  });

  it("returns what it has when a later request fails, with a next that retries it", async () => {
    const pager = new Pager();
    let fail = true;
    const load = async (): Promise<Chunk<{ id: string }>> => ({
      items: [{ id: "a" }],
      more: async () => {
        if (fail) throw new Error("fetch failed");
        return { items: [{ id: "b" }, { id: "c" }] };
      },
    });
    const p1 = await pager.page("video/home", load, byId);
    expect(ids(p1)).toEqual(["a"]);
    fail = false;
    expect(ids(await pager.resume("video/home", p1.next!)!())).toEqual(["b", "c"]);
  });

  it("fails the page when its first request fails", async () => {
    const pager = new Pager();
    await expect(pager.page("video/home", () => Promise.reject(new Error("boom")), byId)).rejects.toThrow("boom");
  });
});
