// On-demand-profile DASH manifest over YouTube's own adaptive MP4 streams (Appendix A).

import { AUDIO_ITAG, DASH_VIDEO_ITAGS, type ResolvedVideo, type StreamFormat } from "./resolver.js";
import type { SegmentRanges } from "./sidx.js";

function xml(value: string | number): string {
  return String(value).replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/"/g, "&quot;");
}

export interface MpdInput {
  video: ResolvedVideo;
  ranges: Map<number, SegmentRanges>;
  publicBase: string;
  token?: string; // echoed into BaseURLs only for desktop players that authenticated with ?token=
}

function representation(f: StreamFormat, r: SegmentRanges, baseUrl: string): string {
  const size = f.mimeType === "video/mp4" ? ` width="${f.width ?? 0}" height="${f.height ?? 0}"` : "";
  const asr = f.mimeType === "audio/mp4" ? ` audioSamplingRate="${f.audioSamplingRate ?? 44100}"` : "";
  return [
    `      <Representation id="${f.itag}" codecs="${xml(f.codecs)}"${size}${asr} bandwidth="${f.bitrate}">`,
    `        <BaseURL>${xml(baseUrl)}</BaseURL>`,
    `        <SegmentBase indexRange="${r.indexRange}"><Initialization range="${r.initRange}"/></SegmentBase>`,
    `      </Representation>`,
  ].join("\n");
}

export function renderMpd({ video, ranges, publicBase, token }: MpdInput): string {
  const base = (itag: number) =>
    `${publicBase}/s/${video.id}/${itag}${token ? `?token=${encodeURIComponent(token)}` : ""}`;
  const reps = (itags: readonly number[]) =>
    itags.flatMap((itag) => {
      const f = video.formats.get(itag);
      const r = ranges.get(itag);
      return f && r ? [representation(f, r, base(itag))] : [];
    });
  const videoReps = reps(DASH_VIDEO_ITAGS);
  const audioReps = reps([AUDIO_ITAG]);
  if (videoReps.length === 0 || audioReps.length === 0) throw new Error(`no DASH streams for ${video.id}`);
  return [
    `<?xml version="1.0" encoding="UTF-8"?>`,
    `<MPD xmlns="urn:mpeg:dash:schema:mpd:2011" type="static"`,
    `     profiles="urn:mpeg:dash:profile:isoff-on-demand:2011"`,
    `     mediaPresentationDuration="PT${video.duration}S" minBufferTime="PT2S">`,
    `  <Period>`,
    `    <AdaptationSet mimeType="video/mp4" segmentAlignment="true" startWithSAP="1">`,
    ...videoReps,
    `    </AdaptationSet>`,
    `    <AdaptationSet mimeType="audio/mp4" lang="und">`,
    ...audioReps,
    `    </AdaptationSet>`,
    `  </Period>`,
    `</MPD>`,
    ``,
  ].join("\n");
}
