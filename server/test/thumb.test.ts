import { describe, expect, it } from "vitest";
import { isThumbHost, sizedThumbUrl, thumbPx } from "../src/thumb.js";

const sized = (u: string, px: number) => sizedThumbUrl(new URL(u), px).toString();

describe("sizedThumbUrl", () => {
  it("rewrites a w×h token, keeping the other options", () => {
    expect(sized("https://yt3.googleusercontent.com/krM0KEoLIpA=w120-h120-l90-rj", 156)).toBe("https://yt3.googleusercontent.com/krM0KEoLIpA=w156-h156-l90-rj");
    expect(sized("https://lh3.googleusercontent.com/qLhu6Py_4_x=w120-h120-p-l90-rj", 400)).toBe("https://lh3.googleusercontent.com/qLhu6Py_4_x=w400-h400-p-l90-rj");
    // larger than needed: ask for less
    expect(sized("https://yt3.googleusercontent.com/7vvEY-pCZ3R=w544-h544-l90-rj", 156)).toBe("https://yt3.googleusercontent.com/7vvEY-pCZ3R=w156-h156-l90-rj");
  });

  it("keeps the aspect of a non-square token, sizing its short side", () => {
    expect(sized("https://yt3.googleusercontent.com/banner=w1060-h175-p-l90-rj", 350)).toBe("https://yt3.googleusercontent.com/banner=w2120-h350-p-l90-rj");
  });

  it("rewrites s tokens and single w or h tokens", () => {
    expect(sized("https://yt3.ggpht.com/avatar=s88-c-k-c0x00ffffff-no-rj", 240)).toBe("https://yt3.ggpht.com/avatar=s240-c-k-c0x00ffffff-no-rj");
    expect(sized("https://lh3.googleusercontent.com/abc=w60", 300)).toBe("https://lh3.googleusercontent.com/abc=w300");
    expect(sized("https://lh3.googleusercontent.com/abc=h60-rj", 300)).toBe("https://lh3.googleusercontent.com/abc=h300-rj");
  });

  it("does not touch dashes inside the image id", () => {
    expect(sized("https://yt3.googleusercontent.com/a-w12-b=w60-h60-l90-rj", 100)).toBe("https://yt3.googleusercontent.com/a-w12-b=w100-h100-l90-rj");
  });

  it("leaves URLs without a size token alone", () => {
    for (const u of [
      "https://i.ytimg.com/vi/papx4qr7abc/hq720.jpg?sqp=-oaymwEXCNAFEJQDSFryq4qpAwkIARUAAIhCGAE=&rs=AOn4CLC",
      "https://www.gstatic.com/youtube/media/ytm/images/pbg/liked-music-@576.png",
      "https://lh3.googleusercontent.com/abc",
      "https://lh3.googleusercontent.com/abc=rj",
      "https://yt3.ggpht.com/a=s88/photo.jpg",
    ]) {
      expect(sized(u, 300)).toBe(new URL(u).toString());
    }
  });
});

describe("thumbPx", () => {
  it("clamps to 16..720 and ignores what is not a number", () => {
    expect(thumbPx("78")).toBe(78);
    expect(thumbPx("95.4")).toBe(95);
    expect(thumbPx("4")).toBe(16);
    expect(thumbPx("4000")).toBe(720);
    expect(thumbPx("abc")).toBeUndefined();
    expect(thumbPx("")).toBeUndefined();
    expect(thumbPx(undefined)).toBeUndefined();
    expect(thumbPx(["78", "90"])).toBeUndefined();
  });
});

describe("isThumbHost", () => {
  it("allows only Google's image hosts over https", () => {
    expect(isThumbHost(new URL("https://i.ytimg.com/vi/x/hq720.jpg"))).toBe(true);
    expect(isThumbHost(new URL("https://yt3.ggpht.com/a=s88"))).toBe(true);
    expect(isThumbHost(new URL("http://i.ytimg.com/vi/x/hq720.jpg"))).toBe(false);
    expect(isThumbHost(new URL("https://ytimg.com.evil.example/x.jpg"))).toBe(false);
  });
});
