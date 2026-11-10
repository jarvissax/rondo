// Direct mode's /lyrics (server/src/lyrics.ts): YouTube Music's own lyrics when the song has a video id
// (timed lines when YouTube has them), else LRCLIB, asked with the title cleaned of upload noise.
#pragma once
#include <string>

namespace rondo::direct {

// the Lyrics JSON: {"synced": [{t, text}] | null, "plain": … | null, "instrumental", "source"}
std::string lyricsJson(const std::string& title, const std::string& artist, const std::string& album, int durationSec,
                       const std::string& videoId);

}  // namespace rondo::direct
